#define _POSIX_C_SOURCE 200809L

/*
 * libreecho-lived - GPT-Live assistant pipeline.
 *
 * lived is the only process that owns a GPT-Live conversation, and it owns no
 * hardware.  It subscribes to the post-AEC stream and the wake events that
 * waked already produces, keeps a few seconds of that audio in RAM, and opens
 * a speech-to-speech session when the wake word fires.  Model audio goes back
 * out through the central playback bus so volume, mute, AirPlay arbitration
 * and the AEC reference stay where they already are.
 *
 * It never opens ALSA, never runs wake inference, never runs local STT or TTS,
 * and never executes a command a model asked for.  Delegated actions are
 * matched against a fixed allow-list in live_tools.c.
 *
 * Threading: none.  One poll() loop drives the wake socket, the audio stream,
 * the control socket and the session pump, in the same style as the rest of
 * the adapter daemons on a device with about 491 MB of RAM.
 */

#include "adapter.h"
#include "live_audio_out.h"
#include "live_ring.h"
#include "live_session.h"
#include "live_tools.h"
#include "live_transport.h"
#include "voice_stream.h"
#include "../json.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_WAKE_SOCKET LE_ADAPTER_WAKEWORD_SOCK
#define DEFAULT_CONTROL_SOCKET LE_ADAPTER_LIVE_SOCK
#define DEFAULT_AUDIO_BUS "/run/libreecho-audio/system.pcm"
#define DEFAULT_SPEAKER_RATE 48000U
/* One audio frame on the wire is a 24-byte header plus samples. */
#define FRAME_READER_BYTES \
    (LE_VOICE_STREAM_HEADER_BYTES + LE_VOICE_STREAM_MAX_SAMPLES * 2U)
/* Longest a single control reply may take to write before we give up. */
#define LOOP_TIMEOUT_MS 20

static volatile sig_atomic_t running = 1;

static void stop_signal(int signo)
{
    (void)signo;
    running = 0;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)(now.tv_nsec / 1000000L);
}

struct frame_reader {
    unsigned char buffer[FRAME_READER_BYTES];
    size_t used;
};

struct lived_state {
    struct le_live_ring ring;
    struct le_live_session session;
    struct le_live_audio_out output;
    struct le_live_tool_environment tools;
    struct le_live_session_config config;
    struct frame_reader reader;

    int wake_fd;
    int audio_fd;
    int listen_fd;
    char wake_socket[LE_ADAPTER_PATH_MAX];
    char control_socket[LE_ADAPTER_PATH_MAX];
    char audio_bus[LE_ADAPTER_PATH_MAX];
    char mock_scenario[64];
    unsigned int speaker_rate;
    int enabled;

    uint64_t wake_events;
    uint64_t wake_ignored;
    uint64_t frames_in;
    uint64_t samples_in;
    uint64_t last_wake_sample;
    uint64_t next_subscription_retry_ms;
    char last_event[64];
};

/* --- small helpers ------------------------------------------------------ */

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *position = buffer;

    while (size) {
        ssize_t count = write(fd, position, size);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        position += count;
        size -= (size_t)count;
    }
    return 0;
}

static int respond(int fd, unsigned long id, int ok, const char *payload)
{
    char response[LE_ADAPTER_MSG_MAX];
    int length = ok
        ? le_adapter_respond_ok(response, sizeof(response), id, payload)
        : le_adapter_respond_err(response, sizeof(response), id, payload);

    if (length < 0) {
        /*
         * A payload that does not fit the adapter envelope must still produce
         * an answer.  Returning nothing closes the connection and leaves the
         * caller unable to tell a bug from a dead daemon.
         */
        length = le_adapter_respond_err(response, sizeof(response), id,
                                        "response exceeds the adapter limit");
        if (length < 0)
            return -1;
    }
    return write_all(fd, response, (size_t)length);
}

static uint16_t get_u16(const unsigned char *p)
{
    return (uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8);
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const unsigned char *p)
{
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

/* Read one newline-terminated request, bounded, without consuming a frame. */
static int read_line(int fd, char *buffer, size_t size)
{
    size_t used = 0;

    while (used + 1 < size) {
        ssize_t count = read(fd, buffer + used, 1);

        if (count < 0 && errno == EINTR)
            continue;
        if (count != 1)
            return -1;
        if (buffer[used++] == '\n') {
            buffer[used - 1] = '\0';
            return 0;
        }
    }
    return -1;
}

/* --- waked subscriptions ------------------------------------------------ */

static int connect_wake_socket(const char *socket_path, const char *command,
                               const char *args)
{
    struct sockaddr_un address;
    char request[256];
    char response[1024];
    int fd;

    if (strlen(socket_path) >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, socket_path);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
        goto fail;
    if (snprintf(request, sizeof(request),
                 "{\"v\":1,\"id\":1,\"cmd\":\"%s\",\"args\":%s}\n",
                 command, args ? args : "{}") >= (int)sizeof(request))
        goto fail;
    if (write_all(fd, request, strlen(request)) < 0)
        goto fail;
    /*
     * Read the acknowledgement line only.  For stream_audio everything after
     * this newline is binary frames, so the handshake must not over-read.
     */
    if (read_line(fd, response, sizeof(response)) < 0)
        goto fail;
    if (!strstr(response, "\"ok\":true")) {
        errno = EPROTO;
        goto fail;
    }
    return fd;

fail:
    close(fd);
    return -1;
}

static void drop_subscriptions(struct lived_state *state)
{
    if (state->wake_fd >= 0)
        close(state->wake_fd);
    if (state->audio_fd >= 0)
        close(state->audio_fd);
    state->wake_fd = -1;
    state->audio_fd = -1;
    state->reader.used = 0;
}

static int reconnect_subscriptions(struct lived_state *state, uint64_t now_ms)
{
    int wake_fd;
    int audio_fd;

    if (state->wake_fd >= 0 && state->audio_fd >= 0)
        return 0;
    if (now_ms < state->next_subscription_retry_ms)
        return -1;
    drop_subscriptions(state);
    wake_fd = connect_wake_socket(state->wake_socket, "subscribe", "{}");
    audio_fd = connect_wake_socket(state->wake_socket, "stream_audio", "{}");
    if (wake_fd < 0 || audio_fd < 0 || set_nonblocking(wake_fd) < 0 ||
        set_nonblocking(audio_fd) < 0) {
        if (wake_fd >= 0)
            close(wake_fd);
        if (audio_fd >= 0)
            close(audio_fd);
        state->next_subscription_retry_ms = now_ms + 1000U;
        return -1;
    }
    state->wake_fd = wake_fd;
    state->audio_fd = audio_fd;
    state->next_subscription_retry_ms = 0;
    fprintf(stderr, "lived: wake and audio subscriptions connected\n");
    return 0;
}

static void close_session(struct lived_state *state,
                          enum le_live_end_reason reason)
{
    le_live_session_close(&state->session, reason, monotonic_ms());
}

static void on_state_changed(void *context, enum le_live_state session_state)
{
    struct lived_state *state = context;

    snprintf(state->last_event, sizeof(state->last_event), "%s",
             le_live_state_name(session_state));
    fprintf(stderr, "lived: state=%s\n", le_live_state_name(session_state));
}

static int on_output_audio(void *context, const int16_t *samples, size_t count,
                           unsigned int rate)
{
    struct lived_state *state = context;

    return le_live_audio_out_write(&state->output, samples, count, rate);
}

static void on_cancel_output(void *context)
{
    struct lived_state *state = context;

    le_live_audio_out_cancel(&state->output);
}

static int on_dispatch(void *context, const char *tool, const char *arguments,
                       char *result, size_t size)
{
    struct lived_state *state = context;
    int result_code;

    fprintf(stderr, "lived: delegation tool=%s\n", tool ? tool : "(none)");
    result_code = le_live_tools_dispatch(&state->tools, tool, arguments, result,
                                         size);
    return result_code;
}

static const struct le_live_session_ops lived_session_ops = {
    on_output_audio,
    on_cancel_output,
    on_dispatch,
    on_state_changed
};

static int start_session(struct lived_state *state, uint64_t detection_sample)
{
    int result;

    if (!state->enabled) {
        ++state->wake_ignored;
        fprintf(stderr, "lived: wake ignored (mode disabled)\n");
        return 0;
    }
    result = le_live_session_wake(&state->session, detection_sample,
                                  monotonic_ms());
    if (result == 1) {
        ++state->wake_ignored;
    } else if (result == 0) {
        fprintf(stderr, "lived: wake sample=%llu\n",
                (unsigned long long)detection_sample);
    }
    return result;
}

/* --- wake events -------------------------------------------------------- */

static void handle_wake_line(struct lived_state *state, const char *line)
{
    long long detection_sample = 0;
    const char *separator;

    if (!strstr(line, "\"wake_detected\""))
        return;
    ++state->wake_events;
    separator = strstr(line, "\"detection_sample\"");
    if (!separator || json_get_int64(line, "detection_sample",
                                     &detection_sample) != 1) {
        /*
         * A wake we cannot place is worse than no wake: starting a session
         * with an unknown sample would send unrelated audio upstream.
         */
        fprintf(stderr, "lived: wake event without a usable sample\n");
        return;
    }
    state->last_wake_sample = (uint64_t)detection_sample;
    (void)start_session(state, (uint64_t)detection_sample);
}

static void drain_wake_events(struct lived_state *state)
{
    char line[2048];
    size_t used = 0;

    for (;;) {
        ssize_t count = read(state->wake_fd, line + used, 1);

        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (count <= 0) {
            fprintf(stderr, "lived: wake subscription closed; reconnecting\n");
            close_session(state, LE_LIVE_END_TRANSPORT_ERROR);
            drop_subscriptions(state);
            state->next_subscription_retry_ms = monotonic_ms() + 1000U;
            return;
        }
        if (line[used] == '\n') {
            line[used] = '\0';
            if (used)
                handle_wake_line(state, line);
            used = 0;
            continue;
        }
        if (++used >= sizeof(line)) {
            fprintf(stderr, "lived: oversized wake event discarded\n");
            used = 0;
        }
    }
}

/* --- indexed audio ------------------------------------------------------ */

static void handle_audio_frame(struct lived_state *state, uint64_t first_sample,
                               const int16_t *samples, size_t count)
{
    ++state->frames_in;
    state->samples_in += count;
    if (!state->enabled) {
        /*
         * Nothing is buffered while GPT-Live is not the selected mode, so the
         * preroll window cannot hold audio for a mode the user did not pick.
         */
        return;
    }
    (void)le_live_session_feed(&state->session, first_sample, samples, count,
                               monotonic_ms());
}

static void drain_audio(struct lived_state *state)
{
    for (;;) {
        ssize_t count;
        size_t consumed = 0;

        if (state->reader.used < sizeof(state->reader.buffer)) {
            count = read(state->audio_fd,
                         state->reader.buffer + state->reader.used,
                         sizeof(state->reader.buffer) - state->reader.used);
            if (count < 0 && errno == EINTR)
                continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                return;
            if (count <= 0) {
                fprintf(stderr, "lived: audio subscription closed; reconnecting\n");
                close_session(state, LE_LIVE_END_TRANSPORT_ERROR);
                drop_subscriptions(state);
                state->next_subscription_retry_ms = monotonic_ms() + 1000U;
                return;
            }
            state->reader.used += (size_t)count;
        }
        /* Complete frames only; a partial frame stays buffered. */
        while (state->reader.used >= LE_VOICE_STREAM_HEADER_BYTES) {
            const unsigned char *packet = state->reader.buffer;
            uint32_t sample_count;
            size_t frame_bytes;

            if (get_u32(packet) != LE_VOICE_STREAM_MAGIC ||
                get_u16(packet + 4) != LE_VOICE_STREAM_VERSION ||
                get_u32(packet + 20) != 0) {
                fprintf(stderr, "lived: malformed audio frame, resyncing\n");
                state->reader.used = 0;
                break;
            }
            sample_count = get_u32(packet + 16);
            if (sample_count == 0 ||
                sample_count > LE_VOICE_STREAM_MAX_SAMPLES) {
                fprintf(stderr, "lived: audio frame out of range\n");
                state->reader.used = 0;
                break;
            }
            frame_bytes = LE_VOICE_STREAM_HEADER_BYTES +
                          (size_t)sample_count * sizeof(int16_t);
            if (state->reader.used < frame_bytes)
                break;
            {
                int16_t samples[LE_VOICE_STREAM_MAX_SAMPLES];

                memcpy(samples, packet + LE_VOICE_STREAM_HEADER_BYTES,
                       (size_t)sample_count * sizeof(int16_t));
                handle_audio_frame(state, get_u64(packet + 8), samples,
                                   sample_count);
            }
            memmove(state->reader.buffer, packet + frame_bytes,
                    state->reader.used - frame_bytes);
            state->reader.used -= frame_bytes;
            consumed += frame_bytes;
        }
        if (!consumed && state->reader.used < sizeof(state->reader.buffer))
            continue;
        if (!consumed)
            return;
    }
}

/* --- control socket ----------------------------------------------------- */

static void status_json(struct lived_state *state, char *out, size_t size)
{
    char session[1024];
    char output[320];
    /* Host + path + bounded error detail do not fit in the old 320-byte
       scratch. Truncating a nested JSON object makes the whole status reply
       malformed, so reserve the adapter's actual bounded budget. */
    char transport[1024];
    int written;

    le_live_session_status_json(&state->session, session, sizeof(session));
    le_live_audio_out_metrics_json(&state->output, output, sizeof(output));
    transport[0] = '\0';
    if (state->session.transport.ops && state->session.transport.ops->metrics)
        state->session.transport.ops->metrics(&state->session.transport,
                                              transport, sizeof(transport));
    if (!transport[0])
        snprintf(transport, sizeof(transport), "{}");
    written = snprintf(
        out, size,
        "{\"enabled\":%s,\"mode\":\"%s\",\"transport\":\"%s\","
        "\"last_event\":\"%s\",\"wake_events\":%llu,\"wake_ignored\":%llu,"
        "\"last_wake_sample\":%llu,\"frames_in\":%llu,\"samples_in\":%llu,"
        "\"ring_samples\":%u,\"ring_capacity\":%u,"
        "\"session\":%s,\"output\":%s,\"transport_metrics\":%s}",
        state->enabled ? "true" : "false",
        state->enabled ? "gpt-live" : "inactive",
        state->config.transport_ops ? state->config.transport_ops->name : "",
        state->last_event, (unsigned long long)state->wake_events,
        (unsigned long long)state->wake_ignored,
        (unsigned long long)state->last_wake_sample,
        (unsigned long long)state->frames_in,
        (unsigned long long)state->samples_in, (unsigned)state->ring.count,
        (unsigned)LE_LIVE_RING_CAPACITY, session, output, transport);
    if (written < 0 || (size_t)written >= size)
        out[size - 1] = '\0';
}

static int parse_bool(const char *args, const char *key, int *out)
{
    const char *value;

    if (!args || !(value = strstr(args, key)) ||
        !(value = strchr(value, ':')))
        return -1;
    ++value;
    while (*value == ' ')
        ++value;
    if (!strncmp(value, "true", 4)) {
        *out = 1;
        return 0;
    }
    if (!strncmp(value, "false", 5)) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int handle_control(struct lived_state *state, int fd, const char *message)
{
    char command[64];
    char *args = NULL;
    unsigned long id = 0;

    if (le_adapter_parse_request((char *)message, command, sizeof(command),
                                 &args, &id) < 0)
        return respond(fd, id, 0, "malformed request");

    if (!strcmp(command, "status")) {
        char payload[2600];

        status_json(state, payload, sizeof(payload));
        return respond(fd, id, 1, payload);
    }
    if (!strcmp(command, "transcript")) {
        /*
         * Sized so the JSON can never exceed the adapter envelope: a reply that
         * does not fit cannot be sent at all, and a caller that gets silence
         * cannot tell a long conversation from a dead daemon.
         */
        char payload[LE_ADAPTER_MSG_MAX - 64];

        le_live_session_transcript_json(&state->session, payload,
                                        sizeof(payload));
        return respond(fd, id, 1, payload);
    }
    if (!strcmp(command, "tools")) {
        char payload[1024];
        size_t used = 0;
        unsigned int index;
        int written;

        written = snprintf(payload, sizeof(payload), "{\"tools\":[");
        if (written < 0 || (size_t)written >= sizeof(payload))
            return respond(fd, id, 0, "tool list too large");
        used = (size_t)written;
        for (index = 0; le_live_tools_name(index); ++index) {
            written = snprintf(payload + used, sizeof(payload) - used,
                               "%s\"%s\"", index ? "," : "",
                               le_live_tools_name(index));
            if (written < 0 || (size_t)written >= sizeof(payload) - used)
                return respond(fd, id, 0, "tool list too large");
            used += (size_t)written;
        }
        if (used + 3 > sizeof(payload))
            return respond(fd, id, 0, "tool list too large");
        memcpy(payload + used, "]}", 3);
        return respond(fd, id, 1, payload);
    }
    if (!strcmp(command, "set_enabled")) {
        int enabled = 0;

        if (parse_bool(args, "\"enabled\"", &enabled) < 0)
            return respond(fd, id, 0, "enabled must be true or false");
        state->enabled = enabled;
        if (!enabled) {
            close_session(state, LE_LIVE_END_STOPPED);
            le_live_ring_reset(&state->ring);
        }
        fprintf(stderr, "lived: mode %s\n", enabled ? "gpt-live" : "inactive");
        return respond(fd, id, 1, enabled ? "{\"enabled\":true}"
                                          : "{\"enabled\":false}");
    }
    if (!strcmp(command, "stop")) {
        close_session(state, LE_LIVE_END_STOPPED);
        return respond(fd, id, 1, "{\"active\":false}");
    }
    if (!strcmp(command, "wake")) {
        long long sample = 0;

        /*
         * Synthetic wake, matching waked's own `test`, so the pipeline can be
         * exercised end to end on a device without saying the wake word.
         */
        if (!args || json_get_int64(args, "detection_sample", &sample) != 1)
            sample = (long long)le_live_ring_end(&state->ring);
        if (start_session(state, (uint64_t)sample) < 0)
            return respond(fd, id, 0, "session could not be started");
        return respond(fd, id, 1, "{\"started\":true}");
    }
    if (!strcmp(command, "set_mock")) {
        char scenario[64];

        if (!args ||
            json_get_string(args, "scenario", scenario, sizeof(scenario)) < 1)
            return respond(fd, id, 0, "scenario is required");
        if (strlen(scenario) >= 48)
            return respond(fd, id, 0, "scenario name is too long");
        snprintf(state->mock_scenario, sizeof(state->mock_scenario), "%s",
                 scenario);
        /*
         * Take effect on the next wake: a scenario changed mid-session would
         * make one conversation look like two different transports.
         */
        state->config.mock_scenario = state->mock_scenario;
        return respond(fd, id, 1, "{\"scenario\":\"applied\"}");
    }
    return respond(fd, id, 0, "unknown command");
}

static void serve_control(struct lived_state *state, int fd)
{
    char message[LE_ADAPTER_MSG_MAX];

    if (read_line(fd, message, sizeof(message)) < 0)
        return;
    (void)handle_control(state, fd, message);
}

/* --- main loop ---------------------------------------------------------- */

static int run_lived(struct lived_state *state)
{
    while (running) {
        struct pollfd descriptors[3];
        nfds_t count = 0;
        int result;
        int had_wake;
        int had_audio;

        (void)reconnect_subscriptions(state, monotonic_ms());
        had_wake = state->wake_fd >= 0;
        had_audio = state->audio_fd >= 0;

        if (had_wake) {
            descriptors[count].fd = state->wake_fd;
            descriptors[count].events = POLLIN;
            descriptors[count].revents = 0;
            ++count;
        }
        if (had_audio) {
            descriptors[count].fd = state->audio_fd;
            descriptors[count].events = POLLIN;
            descriptors[count].revents = 0;
            ++count;
        }
        descriptors[count].fd = state->listen_fd;
        descriptors[count].events = POLLIN;
        descriptors[count].revents = 0;
        ++count;

        result = poll(descriptors, count, LOOP_TIMEOUT_MS);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            break;

        {
            nfds_t index = 0;

            if (had_wake) {
                if (state->wake_fd >= 0 &&
                    descriptors[index].revents & (POLLIN | POLLHUP | POLLERR))
                    drain_wake_events(state);
                ++index;
            }
            if (had_audio) {
                if (state->audio_fd >= 0 &&
                    descriptors[index].revents & (POLLIN | POLLHUP | POLLERR))
                    drain_audio(state);
                ++index;
            }
            if (descriptors[index].revents & POLLIN) {
                int client = le_adapter_accept(state->listen_fd);

                if (client >= 0) {
                    serve_control(state, client);
                    close(client);
                }
            }
        }

        /*
         * Non-blocking pump: without an explicit pump a session that stopped
         * receiving events would never hit its connect, conversation or
         * maximum-duration bounds.
         */
        (void)le_live_session_pump(&state->session, 0, monotonic_ms());
    }
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--foreground] [--socket PATH] [--wake-socket PATH] "
            "[--audio-bus PATH] [--speaker-rate HZ] "
            "[--conversation-timeout-ms N] [--max-session-ms N] "
            "[--wake-preroll-ms N] [--barge-in-rms N] [--barge-in-factor N] "
            "[--model NAME] [--voice NAME] [--credentials PATH] "
            "[--transport realtime|mock] [--live-url URL] [--live-ca PATH] "
            "[--mock-scenario NAME] [--enable]\n",
            program);
}

int main(int argc, char **argv)
{
    struct lived_state state;
    int i;
    int exit_code = 1;

    memset(&state, 0, sizeof(state));
    state.wake_fd = -1;
    state.audio_fd = -1;
    state.listen_fd = -1;
    snprintf(state.wake_socket, sizeof(state.wake_socket), "%s",
             DEFAULT_WAKE_SOCKET);
    snprintf(state.control_socket, sizeof(state.control_socket), "%s",
             DEFAULT_CONTROL_SOCKET);
    snprintf(state.audio_bus, sizeof(state.audio_bus), "%s", DEFAULT_AUDIO_BUS);
    state.speaker_rate = DEFAULT_SPEAKER_RATE;
    /* The service is installed and available at boot, but only the explicit
       control-centre selection arms it as a wake consumer. */
    state.enabled = 0;
    /*
     * Default to the real transport.  A build that cannot reach GPT-Live must
     * fail closed and say so; silently substituting the mock would let a user
     * believe a conversation had happened.
     */
    state.config.transport_ops = le_live_transport_realtime_ops();
    state.config.conversation_timeout_ms = LE_LIVE_DEFAULT_TIMEOUT_MS;
    state.config.max_session_ms = LE_LIVE_DEFAULT_MAX_SESSION_MS;
    state.config.connect_timeout_ms = LE_LIVE_DEFAULT_CONNECT_TIMEOUT_MS;
    state.config.wake_preroll_ms = LE_LIVE_DEFAULT_WAKE_PREROLL_MS;
    state.config.barge_in_rms = LE_LIVE_DEFAULT_BARGE_IN_RMS;
    state.config.barge_in_frames = LE_LIVE_DEFAULT_BARGE_IN_FRAMES;
    state.config.barge_in_factor = LE_LIVE_DEFAULT_BARGE_IN_FACTOR;
    state.config.model = "gpt-live-1-codex";
    state.config.voice = "cove";
    state.config.credentials_path = "/data/libreecho/secrets/openai-codex.json";
    state.config.ca_path = "/usr/local/share/libreecho/cacert.pem";
    state.config.mock_scenario = "session";

    for (i = 1; i < argc; ++i) {
        const char *option = argv[i];
        const char *value = i + 1 < argc ? argv[i + 1] : NULL;

#define NEXT() (++i, value)
        if (!strcmp(option, "--foreground")) {
            /* Accepted for init-script compatibility; the daemon does not
               detach itself, the supervisor does. */
            continue;
        } else if (!strcmp(option, "--enable")) {
            state.enabled = 1;
        } else if (!strcmp(option, "--socket") && value) {
            snprintf(state.control_socket, sizeof(state.control_socket), "%s",
                     NEXT());
        } else if (!strcmp(option, "--wake-socket") && value) {
            snprintf(state.wake_socket, sizeof(state.wake_socket), "%s",
                     NEXT());
        } else if (!strcmp(option, "--audio-bus") && value) {
            snprintf(state.audio_bus, sizeof(state.audio_bus), "%s", NEXT());
        } else if (!strcmp(option, "--speaker-rate") && value) {
            state.speaker_rate = (unsigned int)strtoul(NEXT(), NULL, 10);
        } else if (!strcmp(option, "--conversation-timeout-ms") && value) {
            state.config.conversation_timeout_ms =
                (unsigned int)strtoul(NEXT(), NULL, 10);
        } else if (!strcmp(option, "--max-session-ms") && value) {
            state.config.max_session_ms =
                (unsigned int)strtoul(NEXT(), NULL, 10);
        } else if (!strcmp(option, "--wake-preroll-ms") && value) {
            state.config.wake_preroll_ms =
                (unsigned int)strtoul(NEXT(), NULL, 10);
        } else if (!strcmp(option, "--barge-in-rms") && value) {
            state.config.barge_in_rms =
                (unsigned int)strtoul(NEXT(), NULL, 10);
        } else if (!strcmp(option, "--barge-in-factor") && value) {
            state.config.barge_in_factor =
                (unsigned int)strtoul(NEXT(), NULL, 10);
        } else if (!strcmp(option, "--model") && value) {
            state.config.model = NEXT();
        } else if (!strcmp(option, "--voice") && value) {
            state.config.voice = NEXT();
        } else if (!strcmp(option, "--credentials") && value) {
            state.config.credentials_path = NEXT();
        } else if (!strcmp(option, "--transport") && value) {
            const char *name = NEXT();

            if (!strcmp(name, "mock"))
                state.config.transport_ops = le_live_transport_mock_ops();
            else if (!strcmp(name, "realtime"))
                state.config.transport_ops = le_live_transport_realtime_ops();
            else {
                fprintf(stderr, "lived: unknown transport '%s'\n", name);
                return 2;
            }
        } else if (!strcmp(option, "--live-url") && value) {
            state.config.url = NEXT();
        } else if (!strcmp(option, "--live-ca") && value) {
            state.config.ca_path = NEXT();
        } else if (!strcmp(option, "--mock-scenario") && value) {
            state.config.mock_scenario = NEXT();
        } else {
            usage(argv[0]);
            return 2;
        }
#undef NEXT
    }

    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    signal(SIGPIPE, SIG_IGN);

    le_live_ring_reset(&state.ring);
    le_live_audio_out_init(&state.output, state.audio_bus, state.speaker_rate);
    le_live_tools_init(&state.tools);
    le_live_session_init(&state.session, &state.ring, &state.config,
                         &lived_session_ops, &state);
    /*
     * Publish the transport to the session before the first wake so the status
     * socket can report what the transport is and why it is unusable - the
     * answer a user needs before anything wakes, not only after a failed one.
     */
    state.session.transport.ops = state.config.transport_ops;
    snprintf(state.mock_scenario, sizeof(state.mock_scenario), "%s",
             state.config.mock_scenario ? state.config.mock_scenario
                                        : "session");
    state.config.mock_scenario = state.mock_scenario;

    state.listen_fd = le_adapter_listen(state.control_socket);
    if (state.listen_fd < 0) {
        perror("lived: control socket");
        goto out;
    }
    snprintf(state.last_event, sizeof(state.last_event), "idle");

    if (reconnect_subscriptions(&state, monotonic_ms()) < 0)
        fprintf(stderr, "lived: warning: wake/audio subscriptions unavailable; retrying\n");

    fprintf(stderr,
            "lived: running (mode=%s transport=%s model=%s voice=%s "
            "timeout=%ums max=%ums preroll=%ums barge=%ux%u/%u socket=%s)\n",
            state.enabled ? "gpt-live" : "inactive",
            state.config.transport_ops->name, state.config.model,
            state.config.voice ? state.config.voice : "default",
            state.config.conversation_timeout_ms, state.config.max_session_ms,
            state.config.wake_preroll_ms, state.config.barge_in_rms,
            state.config.barge_in_factor, state.config.barge_in_frames,
            state.control_socket);

    exit_code = run_lived(&state);

out:
    close_session(&state, LE_LIVE_END_STOPPED);
    le_live_audio_out_close(&state.output);
    if (state.wake_fd >= 0)
        close(state.wake_fd);
    if (state.audio_fd >= 0)
        close(state.audio_fd);
    if (state.listen_fd >= 0)
        close(state.listen_fd);
    unlink(state.control_socket);
    fprintf(stderr,
            "lived: frames=%llu samples=%llu wakes=%llu ignored=%llu\n",
            (unsigned long long)state.frames_in,
            (unsigned long long)state.samples_in,
            (unsigned long long)state.wake_events,
            (unsigned long long)state.wake_ignored);
    return exit_code;
}
