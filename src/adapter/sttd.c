#define _POSIX_C_SOURCE 200809L

#include "adapter.h"
#include "stt_engine.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_STT_SOCKET "/run/libreecho/stt.sock"
#define DEFAULT_STT_MODEL "/usr/local/share/libreecho/stt"
#define STT_RATE 16000U
#define STT_MAX_SECONDS 20U
#define STT_IDLE_TIMEOUT_MS 15000
#define STT_TEXT_MAX 4096U

static volatile sig_atomic_t running = 1;

static void signal_handler(int signal_number)
{
    (void)signal_number;
    running = 0;
}

static uint64_t monotonic_milliseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000ULL +
           (uint64_t)now.tv_nsec / 1000000ULL;
}

static int write_all(int fd, const void *buffer, size_t size)
{
    const unsigned char *p = buffer;

    while (size) {
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

static size_t json_escape(char *output, size_t output_size,
                          const char *input)
{
    const unsigned char *p =
        (const unsigned char *)(input ? input : "");
    size_t used = 0;

    while (*p && used + 2 < output_size) {
        const char *replacement = NULL;
        unsigned char c = *p++;

        switch (c) {
        case '\\': replacement = "\\\\"; break;
        case '"': replacement = "\\\""; break;
        case '\n': replacement = "\\n"; break;
        case '\r': replacement = "\\r"; break;
        case '\t': replacement = "\\t"; break;
        default: break;
        }
        if (replacement) {
            size_t length = strlen(replacement);

            if (used + length >= output_size)
                break;
            memcpy(output + used, replacement, length);
            used += length;
        } else if (c >= 0x20) {
            output[used++] = (char)c;
        }
    }
    if (output_size)
        output[used] = '\0';
    return used;
}

static int publish_transcript_event(int fd, const char *event_name,
                                    const char *text, int final,
                                    uint64_t audio_samples,
                                    uint64_t processing_ms,
                                    uint64_t total_ms,
                                    int endpoint)
{
    char escaped[STT_TEXT_MAX * 2U];
    char data[STT_TEXT_MAX * 2U + 256U];
    char event[STT_TEXT_MAX * 2U + 512U];
    int length;

    json_escape(escaped, sizeof(escaped), text);
    if (snprintf(
            data, sizeof(data),
            "{\"text\":\"%s\",\"final\":%s,\"endpoint\":%s,"
            "\"audio_ms\":%llu,\"processing_ms\":%llu,"
            "\"total_ms\":%llu}",
            escaped, final ? "true" : "false",
            endpoint ? "true" : "false",
            (unsigned long long)(audio_samples * 1000ULL / STT_RATE),
            (unsigned long long)processing_ms,
            (unsigned long long)total_ms) >= (int)sizeof(data))
        return -1;
    length = le_adapter_format_event(
        event, sizeof(event), event_name, data);
    return length < 0
        ? -1 : write_all(fd, event, (size_t)length);
}

static int recognize_stream(struct stt_engine *engine, int fd)
{
    struct stt_stream *stream;
    unsigned char bytes[4096];
    unsigned char pending = 0;
    int have_pending = 0;
    uint64_t audio_samples = 0;
    uint64_t started = monotonic_milliseconds();
    uint64_t last_audio = started;
    uint64_t processing_started;
    uint64_t completed;
    char text[STT_TEXT_MAX];
    char last_partial[STT_TEXT_MAX];
    int endpoint = 0;
    int failed = 0;

    stream = stt_engine_stream_create(engine);
    if (!stream)
        return -1;
    text[0] = '\0';
    last_partial[0] = '\0';
    while (running && audio_samples < STT_RATE * STT_MAX_SECONDS) {
        struct pollfd descriptor;
        ssize_t count;
        int poll_result;
        size_t offset = 0;
        size_t sample_count;
        int16_t samples[(sizeof(bytes) + 1U) / 2U];

        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        poll_result = poll(&descriptor, 1, 1000);
        if (poll_result < 0 && errno == EINTR)
            continue;
        if (poll_result < 0) {
            failed = 1;
            break;
        }
        if (poll_result == 0) {
            uint64_t now = monotonic_milliseconds();

            if (now - last_audio >= STT_IDLE_TIMEOUT_MS) {
                failed = 1;
                break;
            }
            continue;
        }
        count = read(fd, bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            failed = 1;
            break;
        }
        if (count == 0)
            break;
        last_audio = monotonic_milliseconds();
        sample_count = 0;
        if (have_pending) {
            samples[sample_count++] =
                (int16_t)((uint16_t)pending |
                          ((uint16_t)bytes[0] << 8));
            offset = 1;
            have_pending = 0;
        }
        while (offset + 1U < (size_t)count) {
            samples[sample_count++] =
                (int16_t)((uint16_t)bytes[offset] |
                          ((uint16_t)bytes[offset + 1U] << 8));
            offset += 2U;
        }
        if (offset < (size_t)count) {
            pending = bytes[offset];
            have_pending = 1;
        }
        if (sample_count) {
            int result = stt_engine_stream_accept(
                stream, samples, sample_count, text, sizeof(text));

            audio_samples += sample_count;
            if (result < 0) {
                failed = 1;
                break;
            }
            if (text[0] && strcmp(text, last_partial)) {
                if (publish_transcript_event(
                        fd, "transcript_partial", text, 0,
                        audio_samples,
                        monotonic_milliseconds() - started,
                        monotonic_milliseconds() - started, 0) < 0) {
                    failed = 1;
                    break;
                }
                snprintf(
                    last_partial, sizeof(last_partial), "%s", text);
            }
            if (result > 0) {
                endpoint = 1;
                break;
            }
        }
    }
    processing_started = monotonic_milliseconds();
    if (!failed &&
        stt_engine_stream_finish(stream, text, sizeof(text)) < 0)
        failed = 1;
    completed = monotonic_milliseconds();
    if (!failed)
        failed = publish_transcript_event(
            fd, "transcript", text, 1, audio_samples,
            completed - processing_started,
            completed - started, endpoint) < 0;
    stt_engine_stream_destroy(stream);
    return failed ? -1 : 0;
}

static void handle_client(struct stt_engine *engine, int client_fd)
{
    char message[LE_ADAPTER_MSG_MAX];
    char command[64];
    char *args = NULL;
    unsigned long id = 0;

    if (read_request_line(
            client_fd, message, sizeof(message)) < 0 ||
        le_adapter_parse_request(
            message, command, sizeof(command), &args, &id) < 0) {
        (void)respond(client_fd, id, 0, "malformed request");
        return;
    }
    (void)args;
    if (!strcmp(command, "status")) {
        char status[256];

        snprintf(
            status, sizeof(status),
            "{\"ready\":true,\"streaming\":true,"
            "\"engine\":\"%s\",\"sample_rate\":16000,"
            "\"format\":\"pcm_s16_le\"}",
            stt_engine_name(engine));
        (void)respond(client_fd, id, 1, status);
    } else if (!strcmp(command, "recognize_stream")) {
        if (respond(
                client_fd, id, 1,
                "{\"streaming\":true,\"sample_rate\":16000,"
                "\"channels\":1,\"format\":\"pcm_s16_le\","
                "\"max_seconds\":20}") == 0)
            (void)recognize_stream(engine, client_fd);
    } else {
        (void)respond(client_fd, id, 0, "unknown command");
    }
}

int main(int argc, char **argv)
{
    const char *socket_path = DEFAULT_STT_SOCKET;
    const char *model_dir = DEFAULT_STT_MODEL;
    /*
     * The Cortex-A53 cluster is fastest with two inference threads for this
     * small streaming Zipformer. Four ORT threads oversubscribe the shared
     * memory/cache path and more than double observed latency.
     */
    unsigned int threads = 2;
    struct stt_engine *engine;
    int listen_fd;
    int i;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--socket") && i + 1 < argc)
            socket_path = argv[++i];
        else if (!strcmp(argv[i], "--model-dir") && i + 1 < argc)
            model_dir = argv[++i];
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
            char *end;
            unsigned long parsed = strtoul(argv[++i], &end, 10);

            if (*end || parsed < 1 || parsed > 4)
                return 2;
            threads = (unsigned int)parsed;
        } else {
            fprintf(
                stderr,
                "usage: %s [--socket PATH] [--model-dir DIR] "
                "[--threads 1..4]\n", argv[0]);
            return 2;
        }
    }
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
    engine = stt_engine_init(model_dir, threads);
    if (!engine) {
        fprintf(stderr, "sttd: unable to load model from %s\n",
                model_dir);
        return 1;
    }
    listen_fd = le_adapter_listen(socket_path);
    if (listen_fd < 0) {
        perror("sttd: listen");
        stt_engine_destroy(engine);
        return 1;
    }
    fprintf(stderr,
            "sttd: ready engine=%s threads=%u socket=%s\n",
            stt_engine_name(engine), threads, socket_path);
    while (running) {
        int client_fd = le_adapter_accept(listen_fd);

        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        handle_client(engine, client_fd);
        close(client_fd);
    }
    close(listen_fd);
    unlink(socket_path);
    stt_engine_destroy(engine);
    return running ? 1 : 0;
}
