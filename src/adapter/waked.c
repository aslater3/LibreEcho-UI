#define _POSIX_C_SOURCE 200809L

#include "adapter.h"
#include "voice_aec.h"
#include "voice_dsp.h"
#include "voice_reference.h"
#include "voice_stream.h"
#include "wake_led.h"
#include "wake_worker.h"

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

#define DEFAULT_MIC_SOCKET "/run/libreecho/mic.sock"
#define DEFAULT_WAKE_SOCKET LE_ADAPTER_WAKEWORD_SOCK
#define DEFAULT_LED_SOCKET LE_ADAPTER_LED_SOCK
#define DEFAULT_REFERENCE_SOCKET \
    "/run/libreecho-audio/aec-reference.sock"
#define DEFAULT_MODEL_DIRECTORY \
    "/usr/share/libreecho/openwakeword"
#define MIC_BUFFER_BYTES (LE_VOICE_AEC_FRAME_SAMPLES * 2U * 8U)
#define PREROLL_SAMPLES (LE_VOICE_AEC_RATE * 3U)
#define REFERENCE_TIMEOUT_NS 250000000ULL
#define WAKE_ACCEPT_THRESHOLD 0.55f
#define MAX_WAKE_SUBSCRIBERS 4U
#define MAX_AUDIO_SUBSCRIBERS 2U

struct waked_config {
    const char *socket_path;
    const char *led_socket;
    const char *mic_socket;
    const char *reference_socket;
    const char *model_directory;
    const char *dump_path;
    unsigned int dump_seconds;
    unsigned int wake_threads;
    unsigned int vad_floor_rms;
    float wake_threshold;
};

struct waked_ipc {
    int listen_fd;
    int event_pipe[2];
    int subscribers[MAX_WAKE_SUBSCRIBERS];
    int audio_subscribers[MAX_AUDIO_SUBSCRIBERS];
    unsigned int detected_count;
    int sensitivity;
    struct le_wake_event last_event;
    struct le_wake_led led;
};

struct waked_metrics {
    uint64_t processed_frames;
    uint64_t aec_frames;
    uint64_t vad_active_frames;
    uint64_t vad_noise_energy;
    uint64_t dump_samples;
    uint64_t wake_scores;
    uint64_t wake_events;
    unsigned int max_inference_us;
    float max_wake_score;
    int vad_active;
    int playback_active;
    unsigned int vad_floor_rms;
};

static volatile sig_atomic_t running = 1;

static void signal_handler(int signo)
{
    (void)signo;
    running = 0;
}

static uint64_t monotonic_nanoseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000000000ULL +
           (uint64_t)now.tv_nsec;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *p = buffer;

    while (size > 0) {
        ssize_t count = write(fd, p, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        p += count;
        size -= (size_t)count;
    }
    return 0;
}

static int read_request_line(int fd, char *buffer, size_t size)
{
    size_t used = 0;

    while (used + 1 < size) {
        struct pollfd descriptor;
        ssize_t count;

        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        if (poll(&descriptor, 1, 250) <= 0)
            return -1;
        count = read(fd, buffer + used, 1);
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

static int respond(int fd, unsigned long id, int ok,
                   const char *payload)
{
    char response[LE_ADAPTER_MSG_MAX];
    int length = ok
        ? le_adapter_respond_ok(
              response, sizeof(response), id, payload)
        : le_adapter_respond_err(
              response, sizeof(response), id, payload);

    return length < 0
        ? -1 : write_all(fd, response, (size_t)length);
}

static int connect_mic_stream(const char *socket_path)
{
    static const char request[] =
        "{\"v\":1,\"id\":1,\"cmd\":\"stream_mono\",\"args\":{}}\n";
    struct sockaddr_un address;
    char response[1024];
    size_t used = 0;
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
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        write_all(fd, request, sizeof(request) - 1) < 0)
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
    if (!strstr(response, "\"ok\":true") ||
        !strstr(response, "\"format\":\"pcm_s16_le\"") ||
        !strstr(response, "\"calibration_applied\":true")) {
        errno = EPROTO;
        goto fail;
    }
    if (set_nonblocking(fd) < 0)
        goto fail;
    return fd;

fail:
    close(fd);
    return -1;
}

static void preroll_write(int16_t *preroll, size_t *position,
                          const int16_t *samples, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        preroll[*position] = samples[i];
        *position = (*position + 1) % PREROLL_SAMPLES;
    }
}

#ifdef LE_WAKE_ENGINE_ONNX
static float sensitivity_threshold(int sensitivity)
{
    float threshold = 0.85f - (float)sensitivity * 0.0044f;

    if (threshold < 0.30f)
        threshold = 0.30f;
    if (threshold > 0.85f)
        threshold = 0.85f;
    return threshold;
}
#endif

static int threshold_sensitivity(float threshold)
{
    int sensitivity =
        (int)((0.85f - threshold) / 0.0044f + 0.5f);

    if (sensitivity < 0)
        sensitivity = 0;
    if (sensitivity > 100)
        sensitivity = 100;
    return sensitivity;
}

static void publish_wake_event(struct waked_ipc *ipc,
                               const struct le_wake_event *event)
{
    char data[512];
    char message[LE_ADAPTER_MSG_MAX];
    int length;
    size_t i;

    ++ipc->detected_count;
    ipc->last_event = *event;
    if (le_wake_led_trigger(
            &ipc->led, monotonic_nanoseconds()) != LE_ADAPTER_OK)
        fprintf(stderr,
                "waked: warning: unable to start wake LED indicator\n");
    fprintf(stderr,
            "waked: wake_event sample=%llu score=%.6f "
            "vad_score=%.3f playback=%s model=%s\n",
            (unsigned long long)event->detection_sample,
            event->score, event->vad_score,
            event->playback_active ? "true" : "false",
            event->model_name);
    if (snprintf(
            data, sizeof(data),
            "{\"detection_sample\":%llu,\"score\":%.6f,"
            "\"vad_score\":%.3f,\"playback_active\":%s,"
            "\"model\":\"%s\"}",
            (unsigned long long)event->detection_sample,
            event->score, event->vad_score,
            event->playback_active ? "true" : "false",
            event->model_name) >= (int)sizeof(data))
        return;
    length = le_adapter_format_event(
        message, sizeof(message), "wake_detected", data);
    if (length < 0)
        return;
    for (i = 0; i < MAX_WAKE_SUBSCRIBERS; ++i) {
        int fd = ipc->subscribers[i];

        if (fd < 0)
            continue;
        if (send(fd, message, (size_t)length,
                 MSG_DONTWAIT | MSG_NOSIGNAL) != length) {
            close(fd);
            ipc->subscribers[i] = -1;
        }
    }
}

#ifdef LE_WAKE_ENGINE_ONNX
static void wake_event_received(
    const struct le_wake_event *event, void *opaque)
{
    struct waked_ipc *ipc = opaque;

    if (write(ipc->event_pipe[1], event, sizeof(*event)) < 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK)
        running = 0;
}
#endif

static int parse_sensitivity(const char *args, int *sensitivity)
{
    const char *value = strstr(args, "\"sensitivity\"");
    char *end;
    long parsed;

    if (!value || !(value = strchr(value, ':')))
        return -1;
    errno = 0;
    parsed = strtol(value + 1, &end, 10);
    if (errno || end == value + 1 || parsed < 0 || parsed > 100)
        return -1;
    *sensitivity = (int)parsed;
    return 0;
}

static int add_subscriber(struct waked_ipc *ipc, int client_fd)
{
    size_t i;

    for (i = 0; i < MAX_WAKE_SUBSCRIBERS; ++i) {
        if (ipc->subscribers[i] < 0) {
            ipc->subscribers[i] = client_fd;
            return 0;
        }
    }
    return -1;
}

static int add_audio_subscriber(struct waked_ipc *ipc, int client_fd)
{
    size_t i;

    if (set_nonblocking(client_fd) < 0)
        return -1;
    for (i = 0; i < MAX_AUDIO_SUBSCRIBERS; ++i) {
        if (ipc->audio_subscribers[i] < 0) {
            ipc->audio_subscribers[i] = client_fd;
            return 0;
        }
    }
    return -1;
}

static void publish_audio_frame(struct waked_ipc *ipc,
                                uint64_t first_sample,
                                const int16_t *samples,
                                size_t count)
{
    size_t i;

    for (i = 0; i < MAX_AUDIO_SUBSCRIBERS; ++i) {
        int fd = ipc->audio_subscribers[i];

        if (fd < 0)
            continue;
        if (le_voice_stream_write_frame(
                fd, first_sample, samples, count, 0) < 0) {
            close(fd);
            ipc->audio_subscribers[i] = -1;
        }
    }
}

static void handle_control_client(
    struct waked_ipc *ipc,
    struct le_wake_worker *wake_worker,
    const struct waked_metrics *metrics,
    int client_fd)
{
    char message[LE_ADAPTER_MSG_MAX];
    char command[64];
    char status[1024];
    char *args = NULL;
    unsigned long id = 0;

#ifndef LE_WAKE_ENGINE_ONNX
    (void)wake_worker;
#endif
    if (read_request_line(
            client_fd, message, sizeof(message)) < 0 ||
        le_adapter_parse_request(
            message, command, sizeof(command), &args, &id) < 0) {
        (void)respond(client_fd, id, 0, "malformed request");
        close(client_fd);
        return;
    }
    if (!strcmp(command, "status")) {
        snprintf(
            status, sizeof(status),
            "{\"enabled\":true,\"wake_word\":\"Alexa\","
            "\"model_status\":\"loaded\",\"sensitivity\":%d,"
            "\"cooldown_ms\":1750,\"detected_count\":%u,"
            "\"cpu_cost\":17,\"memory_cost_mb\":6,"
            "\"vad_active\":%s,\"vad_floor_rms\":%u,"
            "\"vad_noise_energy\":%llu,\"aec_active\":%s,"
            "\"model\":\"alexa_v0.1\"}",
            ipc->sensitivity, ipc->detected_count,
            metrics->vad_active ? "true" : "false",
            metrics->vad_floor_rms,
            (unsigned long long)metrics->vad_noise_energy,
            metrics->playback_active ? "true" : "false");
        (void)respond(client_fd, id, 1, status);
    } else if (!strcmp(command, "subscribe")) {
        if (add_subscriber(ipc, client_fd) < 0) {
            (void)respond(client_fd, id, 0,
                          "subscriber limit reached");
            close(client_fd);
            return;
        }
        if (respond(client_fd, id, 1,
                    "{\"subscribed\":true}") < 0) {
            size_t i;
            for (i = 0; i < MAX_WAKE_SUBSCRIBERS; ++i) {
                if (ipc->subscribers[i] == client_fd)
                    ipc->subscribers[i] = -1;
            }
            close(client_fd);
        }
        return;
    } else if (!strcmp(command, "stream_audio")) {
        if (add_audio_subscriber(ipc, client_fd) < 0) {
            (void)respond(client_fd, id, 0,
                          "audio subscriber limit reached");
            close(client_fd);
            return;
        }
        if (respond(
                client_fd, id, 1,
                "{\"streaming\":true,\"format\":\"pcm_s16_le\","
                "\"sample_rate\":16000,\"channels\":1,"
                "\"frame_header_bytes\":24,"
                "\"sample_indexed\":true}") < 0) {
            size_t i;

            for (i = 0; i < MAX_AUDIO_SUBSCRIBERS; ++i) {
                if (ipc->audio_subscribers[i] == client_fd)
                    ipc->audio_subscribers[i] = -1;
            }
            close(client_fd);
        }
        return;
    } else if (!strcmp(command, "set_sensitivity")) {
        int sensitivity;

        if (parse_sensitivity(args, &sensitivity) < 0
#ifdef LE_WAKE_ENGINE_ONNX
            ||
            le_wake_worker_set_threshold(
                wake_worker,
                sensitivity_threshold(sensitivity)) < 0
#endif
        )
            (void)respond(client_fd, id, 0,
                          "invalid sensitivity");
        else {
            ipc->sensitivity = sensitivity;
            (void)respond(client_fd, id, 1, "{}");
        }
    } else if (!strcmp(command, "set_word")) {
        if (!strstr(args, "\"Alexa\"") &&
            !strstr(args, "\"alexa\""))
            (void)respond(client_fd, id, 0,
                          "only the Alexa development model is installed");
        else
            (void)respond(client_fd, id, 1, "{}");
    } else if (!strcmp(command, "test")) {
        const struct le_wake_event event = {
            metrics->processed_frames *
                LE_VOICE_AEC_FRAME_SAMPLES,
            1.0f, 1.0f, 0, "synthetic-test"
        };

        publish_wake_event(ipc, &event);
        (void)respond(client_fd, id, 1,
                      "{\"detected\":true}");
    } else {
        (void)respond(client_fd, id, 0, "unknown command");
    }
    close(client_fd);
}

static int process_frame(struct le_voice_aec *aec,
                         struct le_voice_reference *reference,
                         struct le_voice_vad *vad,
                         struct le_wake_worker *wake_worker,
                         struct waked_ipc *ipc,
                         const int16_t *microphone,
                         int16_t *preroll,
                         size_t *preroll_position,
                         int dump_fd,
                         uint64_t dump_limit,
                         struct waked_metrics *metrics)
{
    int16_t far_end[LE_VOICE_AEC_FRAME_SAMPLES];
    int16_t clean[LE_VOICE_AEC_FRAME_SAMPLES];
    struct le_voice_vad_result vad_result;
    float vad_score;
    uint64_t now_ns = monotonic_nanoseconds();
    int playback_active = le_voice_reference_active(
        reference, now_ns, REFERENCE_TIMEOUT_NS);
    size_t dump_count = LE_VOICE_AEC_FRAME_SAMPLES;

    metrics->playback_active = playback_active;
    if (playback_active) {
        le_voice_reference_read(reference, far_end,
                                LE_VOICE_AEC_FRAME_SAMPLES);
        le_voice_aec_process(aec, microphone, far_end, clean);
        ++metrics->aec_frames;
    } else {
        memset(far_end, 0, sizeof(far_end));
        memcpy(clean, microphone, sizeof(clean));
    }
    vad_result = le_voice_vad_process(
        vad, clean, LE_VOICE_AEC_FRAME_SAMPLES);
    vad_score = vad_result.noise_energy
        ? (float)vad_result.frame_energy /
          (float)(vad_result.noise_energy * 6U)
        : 0.0f;
    if (vad_score > 1.0f)
        vad_score = 1.0f;
    metrics->vad_active = vad_result.active;
    metrics->vad_noise_energy = vad_result.noise_energy;
    if (vad_result.active)
        ++metrics->vad_active_frames;
    preroll_write(preroll, preroll_position, clean,
                  LE_VOICE_AEC_FRAME_SAMPLES);
    ++metrics->processed_frames;
    publish_audio_frame(
        ipc,
        (metrics->processed_frames - 1U) *
            LE_VOICE_AEC_FRAME_SAMPLES,
        clean, LE_VOICE_AEC_FRAME_SAMPLES);

#ifdef LE_WAKE_ENGINE_ONNX
    if (wake_worker && wake_worker->implementation) {
        const struct le_wake_observation observation = {
            metrics->processed_frames *
                LE_VOICE_AEC_FRAME_SAMPLES,
            vad_score,
            vad_result.active,
            playback_active
        };

        /*
         * The bounded worker keeps ONNX inference off the 10 ms AFE path.
         * Every frame, including silence, is submitted continuously.
         */
        if (le_wake_worker_submit(
                wake_worker, clean, LE_VOICE_AEC_FRAME_SAMPLES,
                &observation) < 0)
            return -1;
    }
#else
    (void)wake_worker;
#endif

    if (dump_fd < 0 || metrics->dump_samples >= dump_limit)
        return 0;
    if (dump_count > dump_limit - metrics->dump_samples)
        dump_count = (size_t)(dump_limit - metrics->dump_samples);
    if (write_all(dump_fd, clean, dump_count * sizeof(clean[0])) < 0)
        return -1;
    metrics->dump_samples += dump_count;
    return metrics->dump_samples >= dump_limit ? 1 : 0;
}

static int run_waked(const struct waked_config *config)
{
    struct le_voice_reference reference;
    struct le_voice_aec aec;
    struct le_voice_vad vad;
    struct le_wake_worker wake_worker;
    struct le_wake_worker_metrics wake_metrics;
    struct waked_metrics metrics;
    struct waked_ipc ipc;
    unsigned char microphone_bytes[MIC_BUFFER_BYTES];
    size_t microphone_used = 0;
    int16_t *preroll = NULL;
    size_t preroll_position = 0;
    uint64_t dump_limit =
        (uint64_t)config->dump_seconds * LE_VOICE_AEC_RATE;
    int microphone_fd = -1;
    int dump_fd = -1;
    int result = -1;
    size_t i;

    memset(&reference, 0, sizeof(reference));
    reference.fd = -1;
    memset(&aec, 0, sizeof(aec));
    memset(&wake_worker, 0, sizeof(wake_worker));
    memset(&wake_metrics, 0, sizeof(wake_metrics));
    memset(&metrics, 0, sizeof(metrics));
    metrics.vad_floor_rms = config->vad_floor_rms;
    memset(&ipc, 0, sizeof(ipc));
    ipc.listen_fd = -1;
    ipc.event_pipe[0] = -1;
    ipc.event_pipe[1] = -1;
    le_wake_led_init(&ipc.led, config->led_socket);
    ipc.sensitivity =
        threshold_sensitivity(config->wake_threshold);
    for (i = 0; i < MAX_WAKE_SUBSCRIBERS; ++i)
        ipc.subscribers[i] = -1;
    for (i = 0; i < MAX_AUDIO_SUBSCRIBERS; ++i)
        ipc.audio_subscribers[i] = -1;
    preroll = (int16_t *)calloc(PREROLL_SAMPLES, sizeof(*preroll));
    if (!preroll)
        goto out;
    ipc.listen_fd = le_adapter_listen(config->socket_path);
    if (ipc.listen_fd < 0) {
        perror("waked: control socket");
        goto out;
    }
    if (pipe(ipc.event_pipe) < 0 ||
        set_nonblocking(ipc.event_pipe[0]) < 0 ||
        set_nonblocking(ipc.event_pipe[1]) < 0) {
        perror("waked: event pipe");
        goto out;
    }
    if (le_voice_reference_open(&reference,
                                config->reference_socket) < 0) {
        perror("waked: reference socket");
        goto out;
    }
    if (le_voice_aec_init(&aec, LE_VOICE_AEC_FRAME_SAMPLES,
                          LE_VOICE_AEC_DEFAULT_FILTER_SAMPLES) < 0) {
        fprintf(stderr, "waked: unable to initialize AEC\n");
        goto out;
    }
    le_voice_vad_init(&vad);
    if (le_voice_vad_set_floor_rms(
            &vad, config->vad_floor_rms) < 0)
        goto out;
#ifdef LE_WAKE_ENGINE_ONNX
    if (le_wake_worker_start(
            &wake_worker, config->model_directory,
            config->wake_threads, config->wake_threshold,
            wake_event_received, &ipc) < 0)
        goto out;
#endif
    microphone_fd = connect_mic_stream(config->mic_socket);
    if (microphone_fd < 0) {
        perror("waked: microphone stream");
        goto out;
    }
    if (config->dump_path) {
        dump_fd = open(config->dump_path,
                       O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (dump_fd < 0) {
            perror("waked: dump output");
            goto out;
        }
    }
    fprintf(stderr,
            "waked: running (16 kHz, frame=10 ms, AEC tail=200 ms, "
            "preroll=3 s, vad-floor=%u RMS, wake=%s)\n",
            config->vad_floor_rms,
#ifdef LE_WAKE_ENGINE_ONNX
            "openwakeword-onnx"
#else
            "disabled"
#endif
    );

    while (running) {
        struct pollfd descriptors[4];
        int poll_result;

        descriptors[0].fd = microphone_fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = le_voice_reference_fd(&reference);
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        descriptors[2].fd = ipc.listen_fd;
        descriptors[2].events = POLLIN;
        descriptors[2].revents = 0;
        descriptors[3].fd = ipc.event_pipe[0];
        descriptors[3].events = POLLIN;
        descriptors[3].revents = 0;
        poll_result = poll(descriptors, 4, 1000);
        if (poll_result < 0 && errno == EINTR)
            continue;
        if (poll_result < 0)
            break;
        if (le_wake_led_expire(
                &ipc.led, monotonic_nanoseconds()) != LE_ADAPTER_OK)
            fprintf(stderr,
                    "waked: warning: unable to stop wake LED indicator\n");
        if (descriptors[1].revents & POLLIN)
            (void)le_voice_reference_drain(&reference);
        if (descriptors[3].revents & POLLIN) {
            struct le_wake_event event;
            ssize_t count;

            while ((count = read(
                        ipc.event_pipe[0], &event,
                        sizeof(event))) == (ssize_t)sizeof(event))
                publish_wake_event(&ipc, &event);
        }
        if (descriptors[2].revents & POLLIN) {
            int client_fd = le_adapter_accept(ipc.listen_fd);

            if (client_fd >= 0)
                handle_control_client(
                    &ipc, &wake_worker, &metrics, client_fd);
        }
        if (descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            break;
        if (descriptors[0].revents & POLLIN) {
            ssize_t count = read(
                microphone_fd, microphone_bytes + microphone_used,
                sizeof(microphone_bytes) - microphone_used);

            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            if (count <= 0)
                break;
            microphone_used += (size_t)count;
            while (microphone_used >=
                   LE_VOICE_AEC_FRAME_SAMPLES * sizeof(int16_t)) {
                int16_t microphone[LE_VOICE_AEC_FRAME_SAMPLES];
                const size_t frame_bytes =
                    LE_VOICE_AEC_FRAME_SAMPLES * sizeof(int16_t);
                int frame_result;

                memcpy(microphone, microphone_bytes, frame_bytes);
                memmove(microphone_bytes,
                        microphone_bytes + frame_bytes,
                        microphone_used - frame_bytes);
                microphone_used -= frame_bytes;
                frame_result = process_frame(
                    &aec, &reference, &vad, &wake_worker, &ipc, microphone,
                    preroll, &preroll_position, dump_fd, dump_limit,
                    &metrics);
                if (frame_result < 0)
                    goto out;
                if (frame_result > 0) {
                    result = 0;
                    goto out;
                }
            }
        }
    }
    result = running ? -1 : 0;

out:
    le_wake_led_shutdown(&ipc.led);
#ifdef LE_WAKE_ENGINE_ONNX
    le_wake_worker_stop(&wake_worker, &wake_metrics);
    metrics.wake_scores = wake_metrics.scores;
    metrics.wake_events = wake_metrics.events;
    metrics.max_wake_score = wake_metrics.max_score;
    metrics.max_inference_us = wake_metrics.max_inference_us;
#endif
    fprintf(stderr,
            "waked: frames=%llu aec_frames=%llu vad_active_frames=%llu "
            "wake_scores=%llu wake_events=%llu max_wake_score=%.6f "
            "max_inference_us=%u wake_dropped_blocks=%llu "
            "wake_failed=%s "
            "ref_packets=%u ref_underflows=%u ref_overruns=%u "
            "ref_discontinuities=%u\n",
            (unsigned long long)metrics.processed_frames,
            (unsigned long long)metrics.aec_frames,
            (unsigned long long)metrics.vad_active_frames,
            (unsigned long long)metrics.wake_scores,
            (unsigned long long)metrics.wake_events,
            metrics.max_wake_score, metrics.max_inference_us,
            (unsigned long long)wake_metrics.dropped_blocks,
            wake_metrics.failed ? "true" : "false",
            reference.packets, reference.underflows, reference.overruns,
            reference.discontinuities);
    if (dump_fd >= 0)
        close(dump_fd);
    if (microphone_fd >= 0)
        close(microphone_fd);
    for (i = 0; i < MAX_WAKE_SUBSCRIBERS; ++i) {
        if (ipc.subscribers[i] >= 0)
            close(ipc.subscribers[i]);
    }
    for (i = 0; i < MAX_AUDIO_SUBSCRIBERS; ++i) {
        if (ipc.audio_subscribers[i] >= 0)
            close(ipc.audio_subscribers[i]);
    }
    if (ipc.event_pipe[0] >= 0)
        close(ipc.event_pipe[0]);
    if (ipc.event_pipe[1] >= 0)
        close(ipc.event_pipe[1]);
    if (ipc.listen_fd >= 0)
        close(ipc.listen_fd);
    unlink(config->socket_path);
    le_voice_aec_destroy(&aec);
    le_voice_reference_close(&reference);
    free(preroll);
    return result;
}

int main(int argc, char **argv)
{
    struct waked_config config = {
        DEFAULT_WAKE_SOCKET,
        DEFAULT_LED_SOCKET,
        DEFAULT_MIC_SOCKET,
        DEFAULT_REFERENCE_SOCKET,
        DEFAULT_MODEL_DIRECTORY,
        NULL,
        0,
        2,
        LE_VOICE_VAD_DEFAULT_FLOOR_RMS,
        WAKE_ACCEPT_THRESHOLD
    };
    int i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--foreground")) {
            continue;
        } else if (!strcmp(argv[i], "--socket") &&
                   i + 1 < argc) {
            config.socket_path = argv[++i];
        } else if (!strcmp(argv[i], "--led-socket") &&
                   i + 1 < argc) {
            config.led_socket = argv[++i];
        } else if (!strcmp(argv[i], "--mic-socket") && i + 1 < argc) {
            config.mic_socket = argv[++i];
        } else if (!strcmp(argv[i], "--reference-socket") &&
                   i + 1 < argc) {
            config.reference_socket = argv[++i];
        } else if (!strcmp(argv[i], "--model-dir") &&
                   i + 1 < argc) {
            config.model_directory = argv[++i];
        } else if (!strcmp(argv[i], "--wake-threads") &&
                   i + 1 < argc) {
            long threads = strtol(argv[++i], NULL, 10);

            if (threads < 1 || threads > 4) {
                fprintf(stderr, "waked: invalid wake thread count\n");
                return 2;
            }
            config.wake_threads = (unsigned int)threads;
        } else if (!strcmp(argv[i], "--wake-threshold") &&
                   i + 1 < argc) {
            char *end = NULL;
            float threshold = strtof(argv[++i], &end);

            if (!end || *end != '\0' ||
                threshold <= 0.0f || threshold >= 1.0f) {
                fprintf(stderr, "waked: invalid wake threshold\n");
                return 2;
            }
            config.wake_threshold = threshold;
        } else if (!strcmp(argv[i], "--vad-floor-rms") &&
                   i + 1 < argc) {
            char *end = NULL;
            long floor_rms = strtol(argv[++i], &end, 10);

            if (!end || *end != '\0' ||
                floor_rms < 1 || floor_rms > 1024) {
                fprintf(stderr, "waked: invalid VAD floor RMS\n");
                return 2;
            }
            config.vad_floor_rms = (unsigned int)floor_rms;
        } else if (!strcmp(argv[i], "--dump-pcm") && i + 1 < argc) {
            config.dump_path = argv[++i];
        } else if (!strcmp(argv[i], "--dump-seconds") && i + 1 < argc) {
            long seconds = strtol(argv[++i], NULL, 10);

            if (seconds < 1 || seconds > 30) {
                fprintf(stderr, "waked: invalid dump duration\n");
                return 2;
            }
            config.dump_seconds = (unsigned int)seconds;
        } else {
            fprintf(stderr,
                    "Usage: %s [--foreground] [--socket PATH] "
                    "[--mic-socket PATH] "
                    "[--reference-socket PATH] "
                    "[--model-dir PATH] [--wake-threads 1..4] "
                    "[--vad-floor-rms 1..1024] "
                    "[--wake-threshold 0..1] "
                    "[--dump-pcm PATH --dump-seconds N]\n", argv[0]);
            return 2;
        }
    }
    if ((config.dump_path == NULL) != (config.dump_seconds == 0)) {
        fprintf(stderr,
                "waked: --dump-pcm and --dump-seconds are required together\n");
        return 2;
    }
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    return run_waked(&config);
}
