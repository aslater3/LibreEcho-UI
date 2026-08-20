#define _POSIX_C_SOURCE 200809L

#include "adapter/voice_stream.h"
#include "adapter/wyoming_protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TEST_PORT 18700
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

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

static int unix_listener(const char *path)
{
    struct sockaddr_un address;
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strcpy(address.sun_path, path);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int tcp_connect(void)
{
    struct sockaddr_in address;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(TEST_PORT);
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0)
            return fd;
        struct timespec delay = {0, 20000000L};
        nanosleep(&delay, NULL);
    }
    close(fd);
    return -1;
}

static int accept_wake(int listener)
{
    int fd = accept(listener, NULL, NULL);
    char request[256];
    if (fd < 0 || read_line(fd, request, sizeof(request)) < 0 ||
        !strstr(request, "\"cmd\":\"subscribe\"")) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    {
        static const char response[] =
            "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{}}\n";
        CHECK(write_all(fd, response, sizeof(response) - 1) == 0);
    }
    return fd;
}

static int accept_audio(int listener)
{
    int fd = accept(listener, NULL, NULL);
    char request[256];
    if (fd < 0 || read_line(fd, request, sizeof(request)) < 0 ||
        !strstr(request, "\"cmd\":\"stream_audio\"")) {
        if (fd >= 0)
            close(fd);
        return -1;
    }
    {
        static const char response[] =
            "{\"v\":1,\"id\":1,\"ok\":true,\"data\":{}}\n";
        CHECK(write_all(fd, response, sizeof(response) - 1) == 0);
    }
    return fd;
}

int main(void)
{
    char socket_path[108];
    char bus_path[128];
    int wake_listener;
    int wake_fd;
    int audio_listener;
    int audio_fd;
    int client_fd;
    int bus_reader;
    int status;
    pid_t child;
    struct le_wyoming_event event;
    int16_t samples[1280];
    size_t i;

    snprintf(socket_path, sizeof(socket_path),
             "/tmp/libreecho-wyoming-test-%ld.sock", (long)getpid());
    snprintf(bus_path, sizeof(bus_path),
             "/tmp/libreecho-wyoming-test-%ld.pcm", (long)getpid());
    CHECK(mkfifo(bus_path, 0600) == 0);
    bus_reader = open(bus_path, O_RDONLY | O_NONBLOCK);
    CHECK(bus_reader >= 0);
    wake_listener = unix_listener(socket_path);
    CHECK(wake_listener >= 0);
    audio_listener = wake_listener;

    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        execl("./build/libreecho-wyomingd", "libreecho-wyomingd",
              "--foreground", "--port", "18700", "--wake-socket",
              socket_path, "--audio-bus", bus_path, (char *)NULL);
        _exit(127);
    }
    wake_fd = accept_wake(wake_listener);
    CHECK(wake_fd >= 0);
    audio_fd = accept_audio(audio_listener);
    CHECK(audio_fd >= 0);
    client_fd = tcp_connect();
    CHECK(client_fd >= 0);
    CHECK(le_wyoming_read_header(client_fd, &event) == 0);
    CHECK(!strcmp(event.type, "satellite-connected"));
    CHECK(le_wyoming_send(client_fd, "describe", NULL, NULL, 0) == 0);
    CHECK(le_wyoming_read_header(client_fd, &event) == 0);
    CHECK(!strcmp(event.type, "info"));
    CHECK(strstr(event.data, "active_wake_words") != NULL);
    CHECK(le_wyoming_send(client_fd, "run-pipeline",
                          "{\"start_stage\":\"wake\","
                          "\"end_stage\":\"tts\"}", NULL, 0) == 0);

    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
        samples[i] = (int16_t)(i & 31);
    CHECK(le_voice_stream_write_frame(audio_fd, 0, samples,
                                      sizeof(samples) / sizeof(samples[0]),
                                      0) == 0);
    {
        static const char wake_event[] =
            "{\"v\":1,\"event\":\"wake_detected\","
            "\"data\":{\"detection_sample\":0,"
            "\"model\":\"alexa_v0.1\"}}\n";
        CHECK(write_all(wake_fd, wake_event, sizeof(wake_event) - 1) == 0);
    }
    CHECK(le_wyoming_read_header(client_fd, &event) == 0);
    CHECK(!strcmp(event.type, "detection"));
    CHECK(strstr(event.data, "\"name\":\"Alexa\"") != NULL);
    CHECK(le_wyoming_send(client_fd, "run-pipeline",
                          "{\"start_stage\":\"asr\","
                          "\"end_stage\":\"tts\"}", NULL, 0) == 0);
    CHECK(le_wyoming_read_header(client_fd, &event) == 0);
    CHECK(!strcmp(event.type, "audio-start"));
    {
        unsigned char payload[sizeof(samples)];
        int found_chunk = !strcmp(event.type, "audio-chunk");
        int found_started = !strcmp(event.type, "streaming-started");
        if (found_chunk)
            CHECK(le_wyoming_read_payload(client_fd, payload, sizeof(payload),
                                          &event) == 0);
        for (i = 0; !found_chunk && i < 3; ++i) {
            CHECK(le_wyoming_read_header(client_fd, &event) == 0);
            if (!strcmp(event.type, "streaming-started"))
                found_started = 1;
            if (!strcmp(event.type, "audio-chunk")) {
                CHECK(event.payload_length == sizeof(samples));
                CHECK(le_wyoming_read_payload(client_fd, payload,
                                              sizeof(payload), &event) == 0);
                found_chunk = 1;
            }
        }
        CHECK(found_chunk);
        if (!found_started) {
            CHECK(le_wyoming_read_header(client_fd, &event) == 0);
            CHECK(!strcmp(event.type, "streaming-started"));
        }
    }
    {
        const int16_t tts_samples[] = {100, 200, 300, 400};
        unsigned char played[128];
        ssize_t played_bytes;

        CHECK(le_wyoming_send(client_fd, "audio-start",
                              "{\"rate\":22050,\"width\":2,"
                              "\"channels\":1}", NULL, 0) == 0);
        CHECK(le_wyoming_send(client_fd, "audio-chunk",
                              "{\"rate\":22050,\"width\":2,"
                              "\"channels\":1}", tts_samples,
                              sizeof(tts_samples)) == 0);
        CHECK(le_wyoming_send(client_fd, "audio-stop", NULL, NULL, 0) == 0);
        CHECK(le_wyoming_read_header(client_fd, &event) == 0);
        CHECK(!strcmp(event.type, "played"));
        played_bytes = read(bus_reader, played, sizeof(played));
        CHECK(played_bytes > 0 && played_bytes % 4 == 0);
    }

    kill(child, SIGTERM);
    waitpid(child, &status, 0);
    close(client_fd);
    close(bus_reader);
    close(wake_fd);
    close(audio_fd);
    close(wake_listener);
    if (audio_listener != wake_listener)
        close(audio_listener);
    unlink(bus_path);
    unlink(socket_path);
    puts("wyoming daemon: satellite handshake, detection and ASR stream: ok");
    return 0;
}
