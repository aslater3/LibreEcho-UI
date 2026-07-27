#define _POSIX_C_SOURCE 200809L

#include "adapter.h"
#include "voice_stream.h"
#include "voice_listening_led.h"
#include "wyoming_protocol.h"
#include "../json.h"
#include "../log.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_PORT 10700
#define DEFAULT_WAKE_SOCKET LE_ADAPTER_WAKEWORD_SOCK
#define DEFAULT_AUDIO_BUS "/run/libreecho-audio/system.pcm"
#define AUDIO_RATE 16000
#define SPEAKER_RATE 48000
#define RING_SECONDS 3
#define RING_SAMPLES (AUDIO_RATE * RING_SECONDS)
#define PCM_CHUNK_SAMPLES LE_VOICE_STREAM_MAX_SAMPLES
#define MAX_PAYLOAD (256U * 1024U)
#define QUIET_RMS 180U
#define QUIET_AFTER_SAMPLES (AUDIO_RATE * 8 / 10)
#define MIN_STREAM_SAMPLES (AUDIO_RATE / 2)
#define MAX_STREAM_SAMPLES (AUDIO_RATE * 8)

static volatile sig_atomic_t running = 1;

struct wyoming_state {
    int client_fd;
    int wake_fd;
    int audio_fd;
    int output_fd;
    int server_running;
    int detected;
    int streaming;
    uint64_t detection_sample;
    uint64_t stream_samples;
    uint64_t quiet_samples;
    uint64_t ring_first;
    size_t ring_count;
    int16_t ring[RING_SAMPLES];
    char wake_socket[LE_ADAPTER_PATH_MAX];
    char audio_bus[LE_ADAPTER_PATH_MAX];
    int port;
};

static void stop_signal(int signal_number)
{
    (void)signal_number;
    running = 0;
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

static int read_line(int fd, char *buffer, size_t size)
{
    size_t used = 0;

    while (used + 1 < size) {
        char byte;
        ssize_t count = read(fd, &byte, 1);
        if (count < 0 && errno == EINTR)
            continue;
        if (count != 1)
            return -1;
        if (byte == '\n') {
            buffer[used] = '\0';
            return 0;
        }
        buffer[used++] = byte;
    }
    return -1;
}

static int listen_tcp(int port)
{
    struct sockaddr_in address;
    int fd;
    int reuse = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 1) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int accept_client(int listen_fd)
{
    struct sockaddr_in address;
    socklen_t length = sizeof(address);
    int fd = accept(listen_fd, (struct sockaddr *)&address, &length);
    int flags;

    if (fd < 0)
        return -1;
    flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    return fd;
}

static int connect_wake(const char *socket_path, const char *command)
{
    struct sockaddr_un address;
    char request[192];
    char response[1024];
    size_t used = 0;
    int fd;
    int length;

    if (!socket_path || strlen(socket_path) >= sizeof(address.sun_path))
        return -1;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, socket_path);
    length = snprintf(request, sizeof(request),
                      "{\"v\":1,\"id\":1,\"cmd\":\"%s\",\"args\":{}}\n",
                      command);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        length < 0 || (size_t)length >= sizeof(request) ||
        write_all(fd, request, (size_t)length) < 0)
        goto fail;
    while (used + 1 < sizeof(response)) {
        ssize_t count = read(fd, response + used, 1);
        if (count < 0 && errno == EINTR)
            continue;
        if (count != 1)
            goto fail;
        if (response[used++] == '\n')
            break;
    }
    response[used] = '\0';
    if (!strstr(response, "\"ok\":true"))
        goto fail;
    return fd;

fail:
    close(fd);
    return -1;
}

static int send_event(struct wyoming_state *state, const char *type,
                      const char *data, const void *payload, size_t length)
{
    if (state->client_fd < 0)
        return -1;
    return le_wyoming_send(state->client_fd, type, data, payload, length);
}

static void close_client(struct wyoming_state *state)
{
    if (state->streaming)
        le_voice_listening_led_set(0);
    if (state->output_fd >= 0)
        close(state->output_fd);
    if (state->client_fd >= 0)
        close(state->client_fd);
    state->output_fd = -1;
    state->client_fd = -1;
    state->server_running = 0;
    state->detected = 0;
    state->streaming = 0;
}

static void ring_append(struct wyoming_state *state,
                        const struct le_voice_stream_frame *frame)
{
    size_t i;

    if (!state->ring_count ||
        frame->first_sample != state->ring_first + state->ring_count) {
        state->ring_first = frame->first_sample;
        state->ring_count = 0;
    }
    for (i = 0; i < frame->sample_count; ++i) {
        if (state->ring_count == RING_SAMPLES) {
            ++state->ring_first;
            --state->ring_count;
        }
        state->ring[(state->ring_first + state->ring_count) % RING_SAMPLES] =
            frame->samples[i];
        ++state->ring_count;
    }
}

static int send_ring_from(struct wyoming_state *state, uint64_t first_sample)
{
    int16_t samples[PCM_CHUNK_SAMPLES];
    uint64_t end = state->ring_first + state->ring_count;

    if (first_sample < state->ring_first)
        first_sample = state->ring_first;
    while (first_sample < end) {
        size_t count = (size_t)(end - first_sample);
        size_t i;
        if (count > PCM_CHUNK_SAMPLES)
            count = PCM_CHUNK_SAMPLES;
        for (i = 0; i < count; ++i)
            samples[i] = state->ring[(first_sample + i) % RING_SAMPLES];
        if (send_event(state, "audio-chunk",
                       "{\"rate\":16000,\"width\":2,\"channels\":1}",
                       samples, count * sizeof(samples[0])) < 0)
            return -1;
        first_sample += count;
    }
    return 0;
}

static unsigned int frame_rms(const int16_t *samples, size_t count)
{
    uint64_t energy = 0;
    size_t i;
    if (!count)
        return 0;
    for (i = 0; i < count; ++i) {
        int32_t sample = samples[i];
        energy += (uint64_t)(sample < 0 ? -sample : sample);
    }
    return (unsigned int)(energy / count);
}

static int open_audio_bus(struct wyoming_state *state)
{
    if (state->output_fd >= 0)
        return 0;
    state->output_fd = open(state->audio_bus, O_WRONLY | O_CLOEXEC);
    return state->output_fd < 0 ? -1 : 0;
}

static int play_pcm16(struct wyoming_state *state, const unsigned char *payload,
                      size_t length, int rate, int channels)
{
    const int16_t *input = (const int16_t *)payload;
    size_t frames;
    size_t output_frames;
    int16_t *output;
    size_t i;

    if (rate <= 0 || channels <= 0 || channels > 2 || length % 2 ||
        length / 2 < (size_t)channels)
        return -1;
    frames = length / (sizeof(int16_t) * (size_t)channels);
    output_frames = (frames * SPEAKER_RATE + (size_t)rate - 1U) /
        (size_t)rate;
    output = calloc(output_frames * 2U, sizeof(*output));
    if (!output)
        return -1;
    for (i = 0; i < output_frames; ++i) {
        size_t source = (i * (size_t)rate) / SPEAKER_RATE;
        int16_t sample;
        if (source >= frames)
            source = frames - 1;
        sample = input[source * (size_t)channels];
        output[i * 2] = sample;
        output[i * 2 + 1] = channels == 2
            ? input[source * 2 + 1] : sample;
    }
    i = write_all(state->output_fd, output,
                  output_frames * 2U * sizeof(*output));
    free(output);
    return i;
}

static int send_info(struct wyoming_state *state)
{
    static const char info[] =
        "{\"satellite\":{\"name\":\"LibreEcho\",\"area\":\"LibreEcho\","
        "\"has_vad\":true,\"active_wake_words\":[\"Alexa\"],"
        "\"max_active_wake_words\":1,\"supports_trigger\":true},"
        "\"mic\":[{\"name\":\"libreecho-microphone\","
        "\"mic_format\":{\"rate\":16000,\"width\":2,\"channels\":1}}],"
        "\"snd\":[{\"name\":\"libreecho-speaker\","
        "\"snd_format\":{\"rate\":48000,\"width\":2,\"channels\":2}}]}";
    return send_event(state, "info", info, NULL, 0);
}

static int start_stream(struct wyoming_state *state)
{
    uint64_t first;
    if (state->streaming)
        return 0;
    if (send_event(state, "audio-start",
                   "{\"rate\":16000,\"width\":2,\"channels\":1}",
                   NULL, 0) < 0)
        return -1;
    first = state->detection_sample > AUDIO_RATE / 5
        ? state->detection_sample - AUDIO_RATE / 5 : 0;
    if (send_ring_from(state, first) < 0)
        return -1;
    if (send_event(state, "streaming-started", NULL, NULL, 0) < 0)
        return -1;
    state->streaming = 1;
    state->stream_samples = 0;
    state->quiet_samples = 0;
    le_voice_listening_led_set(1);
    return 0;
}

static int stop_stream(struct wyoming_state *state)
{
    if (!state->streaming)
        return 0;
    state->streaming = 0;
    le_voice_listening_led_set(0);
    state->detected = 0;
    state->server_running = 0;
    if (send_event(state, "audio-stop", NULL, NULL, 0) < 0)
        return -1;
    return send_event(state, "streaming-stopped", NULL, NULL, 0);
}

static int handle_server_event(struct wyoming_state *state)
{
    struct le_wyoming_event event;
    unsigned char payload[MAX_PAYLOAD];
    const char *data;

    if (le_wyoming_read_header(state->client_fd, &event) < 0 ||
        event.payload_length > sizeof(payload) ||
        le_wyoming_read_payload(state->client_fd, payload, sizeof(payload),
                                &event) < 0)
        return -1;
    data = event.data_length ? event.data : event.header;
    if (!strcmp(event.type, "describe"))
        return send_info(state);
    if (!strcmp(event.type, "run-satellite")) {
        state->server_running = 1;
        return 0;
    }
    if (!strcmp(event.type, "pause-satellite")) {
        (void)stop_stream(state);
        state->server_running = 0;
        return 0;
    }
    if (!strcmp(event.type, "run-pipeline")) {
        char stage[32];
        if (json_get_string(data, "start_stage", stage,
                            sizeof(stage)) < 1)
            return -1;
        if (!strcmp(stage, "wake")) {
            state->server_running = 1;
            return 0;
        }
        if (!strcmp(stage, "asr") && state->detected)
            return start_stream(state);
        return 0;
    }
    if (!strcmp(event.type, "audio-start")) {
        if (event.payload_length != 0)
            return -1;
        if (state->output_fd >= 0)
            close(state->output_fd);
        state->output_fd = -1;
        return open_audio_bus(state);
    }
    if (!strcmp(event.type, "audio-chunk")) {
        int rate;
        int width;
        int channels;
        if (json_get_int(data, "rate", &rate) != 1 ||
            json_get_int(data, "width", &width) != 1 ||
            json_get_int(data, "channels", &channels) != 1 ||
            width != 2 || state->output_fd < 0)
            return -1;
        return play_pcm16(state, payload, event.payload_length, rate,
                          channels);
    }
    if (!strcmp(event.type, "audio-stop")) {
        if (state->output_fd >= 0)
            close(state->output_fd);
        state->output_fd = -1;
        return send_event(state, "played", NULL, NULL, 0);
    }
    return 0;
}

static int handle_wake_event(struct wyoming_state *state)
{
    char line[LE_ADAPTER_MSG_MAX];
    char model[64];
    const char *position;
    char *end;
    unsigned long long sample;

    if (read_line(state->wake_fd, line, sizeof(line)) < 0)
        return -1;
    if (!strstr(line, "\"event\":\"wake_detected\""))
        return 0;
    position = strstr(line, "\"detection_sample\"");
    if (!position || !(position = strchr(position, ':')))
        return 0;
    sample = strtoull(position + 1, &end, 10);
    if (end == position + 1)
        return 0;
    if (json_get_string(line, "model", model, sizeof(model)) < 1)
        strcpy(model, "Alexa");
    if (state->client_fd >= 0 && state->server_running && !state->detected) {
        char data[128];
        const char *name = !strcmp(model, "alexa_v0.1") ? "Alexa" : model;
        (void)snprintf(data, sizeof(data), "{\"name\":\"%s\","
                       "\"timestamp\":0}", name);
        state->detection_sample = (uint64_t)sample;
        state->detected = 1;
        if (send_event(state, "detection", data, NULL, 0) < 0)
            return -1;
    }
    return 0;
}

static int handle_audio_frame(struct wyoming_state *state)
{
    struct le_voice_stream_frame frame;
    unsigned int rms;

    if (le_voice_stream_read_frame(state->audio_fd, &frame) != 1)
        return -1;
    ring_append(state, &frame);
    if (!state->streaming || state->client_fd < 0)
        return 0;
    if (send_event(state, "audio-chunk",
                   "{\"rate\":16000,\"width\":2,\"channels\":1}",
                   frame.samples, frame.sample_count * sizeof(int16_t)) < 0)
        return -1;
    state->stream_samples += frame.sample_count;
    rms = frame_rms(frame.samples, frame.sample_count);
    if (state->stream_samples >= MIN_STREAM_SAMPLES && rms < QUIET_RMS)
        state->quiet_samples += frame.sample_count;
    else
        state->quiet_samples = 0;
    if (state->stream_samples >= MAX_STREAM_SAMPLES ||
        state->quiet_samples >= QUIET_AFTER_SAMPLES)
        return stop_stream(state);
    return 0;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s [--port N] [--wake-socket PATH] "
            "[--audio-bus PATH] [--foreground]\n", program);
}

int main(int argc, char **argv)
{
    struct wyoming_state state;
    struct sigaction action;
    int listen_fd;
    int i;

    memset(&state, 0, sizeof(state));
    state.client_fd = -1;
    state.wake_fd = -1;
    state.audio_fd = -1;
    state.output_fd = -1;
    state.port = DEFAULT_PORT;
    strcpy(state.wake_socket, DEFAULT_WAKE_SOCKET);
    strcpy(state.audio_bus, DEFAULT_AUDIO_BUS);
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)
            state.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wake-socket") && i + 1 < argc)
            strncpy(state.wake_socket, argv[++i], sizeof(state.wake_socket) - 1);
        else if (!strcmp(argv[i], "--audio-bus") && i + 1 < argc)
            strncpy(state.audio_bus, argv[++i], sizeof(state.audio_bus) - 1);
        else if (!strcmp(argv[i], "--foreground"))
            continue;
        else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (state.port < 1 || state.port > 65535) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    le_log_init("wyomingd", argc, argv);
    memset(&action, 0, sizeof(action));
    action.sa_handler = stop_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    listen_fd = listen_tcp(state.port);
    if (listen_fd < 0) {
        perror("wyomingd: listen");
        return EXIT_FAILURE;
    }
    le_log_info("wyomingd: listening on TCP port %d", state.port);
    while (running) {
        struct pollfd descriptors[4];
        nfds_t count = 1;
        int result;

        if (state.wake_fd < 0)
            state.wake_fd = connect_wake(state.wake_socket, "subscribe");
        if (state.audio_fd < 0)
            state.audio_fd = connect_wake(state.wake_socket, "stream_audio");
        descriptors[0].fd = state.client_fd >= 0 ? state.client_fd : listen_fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        if (state.wake_fd >= 0) {
            descriptors[count].fd = state.wake_fd;
            descriptors[count].events = POLLIN;
            descriptors[count++].revents = 0;
        }
        if (state.audio_fd >= 0) {
            descriptors[count].fd = state.audio_fd;
            descriptors[count].events = POLLIN;
            descriptors[count++].revents = 0;
        }
        result = poll(descriptors, count, 250);
        if (result < 0 && errno == EINTR)
            continue;
        if (result < 0)
            break;
        if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (state.client_fd >= 0)
                close_client(&state);
        } else if (descriptors[0].revents & POLLIN) {
            if (state.client_fd < 0) {
                state.client_fd = accept_client(listen_fd);
                if (state.client_fd >= 0)
                    (void)send_event(&state, "satellite-connected", NULL,
                                     NULL, 0);
            } else if (handle_server_event(&state) < 0) {
                close_client(&state);
            }
        }
        for (i = 1; i < (int)count; ++i) {
            if (!(descriptors[i].revents & POLLIN))
                continue;
            if (descriptors[i].fd == state.wake_fd) {
                if (handle_wake_event(&state) < 0) {
                    close(state.wake_fd);
                    state.wake_fd = -1;
                }
            } else if (descriptors[i].fd == state.audio_fd) {
                if (handle_audio_frame(&state) < 0) {
                    close(state.audio_fd);
                    state.audio_fd = -1;
                }
            }
        }
    }
    close_client(&state);
    if (state.wake_fd >= 0)
        close(state.wake_fd);
    if (state.audio_fd >= 0)
        close(state.audio_fd);
    close(listen_fd);
    return EXIT_SUCCESS;
}
