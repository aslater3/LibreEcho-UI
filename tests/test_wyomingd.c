#define _POSIX_C_SOURCE 200809L

#include "adapter/voice_stream.h"
#include "adapter/wyoming_protocol.h"
#include "json.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
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
    struct timeval timeout = {2, 0};
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)) < 0) {
        close(fd);
        return -1;
    }
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

static int read_event(int fd, struct le_wyoming_event *event)
{
    unsigned char payload[LE_VOICE_STREAM_MAX_SAMPLES * sizeof(int16_t)];

    CHECK(le_wyoming_read_header(fd, event) == 0);
    CHECK(event->payload_length <= sizeof(payload));
    if (event->payload_length)
        CHECK(le_wyoming_read_payload(fd, payload, sizeof(payload), event) == 0);
    return 0;
}

static int send_wake(int fd, uint64_t detection_sample)
{
    char line[256];
    int length = snprintf(line, sizeof(line),
                          "{\"v\":1,\"event\":\"wake_detected\","
                          "\"data\":{\"detection_sample\":%llu,"
                          "\"model\":\"alexa_v0.1\"}}\n",
                          (unsigned long long)detection_sample);

    CHECK(length > 0 && (size_t)length < sizeof(line));
    CHECK(write_all(fd, line, (size_t)length) == 0);
    return 0;
}

static int expect_local_wake_start(int fd)
{
    struct le_wyoming_event event;
    int found_chunk = 0;
    int found_started = 0;
    size_t i;

    CHECK(read_event(fd, &event) == 0);
    CHECK(!strcmp(event.type, "detection"));
    CHECK(strstr(event.data, "\"name\":\"Alexa\"") != NULL);
    CHECK(read_event(fd, &event) == 0);
    CHECK(!strcmp(event.type, "run-pipeline"));
    CHECK(strstr(event.data, "\"start_stage\":\"asr\"") != NULL);
    CHECK(strstr(event.data, "\"end_stage\":\"tts\"") != NULL);
    CHECK(strstr(event.data, "\"restart_on_end\":false") != NULL);
    CHECK(strstr(event.data,
                 "\"snd_format\":{\"rate\":48000,\"width\":2,"
                 "\"channels\":2}") != NULL);
    CHECK(read_event(fd, &event) == 0);
    CHECK(!strcmp(event.type, "audio-start"));
    for (i = 0; i < 4 && (!found_chunk || !found_started); ++i) {
        CHECK(read_event(fd, &event) == 0);
        if (!strcmp(event.type, "audio-chunk"))
            found_chunk = 1;
        else if (!strcmp(event.type, "streaming-started"))
            found_started = 1;
        else
            CHECK(0);
    }
    CHECK(found_chunk && found_started);
    return 0;
}

static int finish_input_stream(int audio_fd, int client_fd,
                               uint64_t first_sample)
{
    int16_t silence[LE_VOICE_STREAM_MAX_SAMPLES] = {0};
    struct le_wyoming_event event;
    int found_audio_stop = 0;
    int found_streaming_stop = 0;
    size_t i;

    for (i = 0; i < 18; ++i) {
        CHECK(le_voice_stream_write_frame(
                  audio_fd,
                  first_sample + i * LE_VOICE_STREAM_MAX_SAMPLES,
                  silence, LE_VOICE_STREAM_MAX_SAMPLES, 0) == 0);
    }
    for (i = 0; i < 24 && (!found_audio_stop || !found_streaming_stop); ++i) {
        CHECK(read_event(client_fd, &event) == 0);
        if (!strcmp(event.type, "audio-stop"))
            found_audio_stop = 1;
        else if (!strcmp(event.type, "streaming-stopped"))
            found_streaming_stop = 1;
        else
            CHECK(!strcmp(event.type, "audio-chunk"));
    }
    CHECK(found_audio_stop && found_streaming_stop);
    return 0;
}

static int play_tts_response(int client_fd, int bus_reader)
{
    const int16_t tts_samples[] = {100, 200, 300, 400};
    unsigned char played[128];
    struct le_wyoming_event event;
    ssize_t played_bytes;

    CHECK(le_wyoming_send(client_fd, "audio-start",
                          "{\"rate\":22050,\"width\":2,"
                          "\"channels\":1}", NULL, 0) == 0);
    CHECK(le_wyoming_send(client_fd, "audio-chunk",
                          "{\"rate\":22050,\"width\":2,"
                          "\"channels\":1}", tts_samples,
                          sizeof(tts_samples)) == 0);
    CHECK(le_wyoming_send(client_fd, "audio-stop", NULL, NULL, 0) == 0);
    CHECK(read_event(client_fd, &event) == 0);
    CHECK(!strcmp(event.type, "played"));
    played_bytes = read(bus_reader, played, sizeof(played));
    CHECK(played_bytes > 0 && played_bytes % 4 == 0);
    return 0;
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
    /* Home Assistant sends this exact startup sequence, then requires a pong
       within five seconds to keep the satellite connection alive. */
    CHECK(le_wyoming_send(client_fd, "run-satellite", NULL, NULL, 0) == 0);
    CHECK(le_wyoming_send(client_fd, "describe", NULL, NULL, 0) == 0);
    CHECK(le_wyoming_read_header(client_fd, &event) == 0);
    CHECK(!strcmp(event.type, "info"));
    CHECK(strstr(event.data, "active_wake_words") != NULL);
    CHECK(json_valid_object(event.data, strlen(event.data)));
    {
        static const char attribution[] =
            "\"attribution\":{\"name\":\"LibreEcho\","
            "\"url\":\"https://libreecho.org\"}";
        const char *satellite = strstr(event.data, "\"satellite\"");
        const char *mic = strstr(event.data, "\"mic\"");
        const char *snd = strstr(event.data, "\"snd\"");
        const char *found;
        CHECK(satellite && mic && snd && satellite < mic && mic < snd);
        found = strstr(satellite, attribution);
        CHECK(found && found < mic);
        found = strstr(mic, attribution);
        CHECK(found && found < snd);
        CHECK(strstr(snd, attribution) != NULL);
        found = strstr(satellite, "\"installed\":true");
        CHECK(found && found < mic);
        found = strstr(mic, "\"installed\":true");
        CHECK(found && found < snd);
        CHECK(strstr(snd, "\"installed\":true") != NULL);
    }
    CHECK(le_wyoming_send(client_fd, "ping", "{\"text\":null}",
                          NULL, 0) == 0);
    CHECK(le_wyoming_read_header(client_fd, &event) == 0);
    CHECK(!strcmp(event.type, "pong"));
    CHECK(!strcmp(event.data, "{\"text\":null}"));
    CHECK(le_wyoming_send(client_fd, "ping",
                          "{\"text\":\"ha-keepalive\"}", NULL, 0) == 0);
    CHECK(le_wyoming_read_header(client_fd, &event) == 0);
    CHECK(!strcmp(event.type, "pong"));
    CHECK(!strcmp(event.data, "{\"text\":\"ha-keepalive\"}"));

    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
        samples[i] = (int16_t)(i & 31);
    CHECK(le_voice_stream_write_frame(audio_fd, 0, samples,
                                      sizeof(samples) / sizeof(samples[0]),
                                      0) == 0);
    CHECK(send_wake(wake_fd, 0) == 0);
    CHECK(expect_local_wake_start(client_fd) == 0);
    CHECK(finish_input_stream(audio_fd, client_fd,
                              LE_VOICE_STREAM_MAX_SAMPLES) == 0);
    CHECK(play_tts_response(client_fd, bus_reader) == 0);

    /* Completing one pipeline must leave the satellite armed for another
       local wake without a reconnect or a second RunSatellite command. */
    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i)
        samples[i] = (int16_t)(31 - (i & 31));
    CHECK(le_voice_stream_write_frame(audio_fd, 50000, samples,
                                      sizeof(samples) / sizeof(samples[0]),
                                      0) == 0);
    CHECK(send_wake(wake_fd, 50000) == 0);
    CHECK(expect_local_wake_start(client_fd) == 0);
    CHECK(finish_input_stream(audio_fd, client_fd,
                              50000 + LE_VOICE_STREAM_MAX_SAMPLES) == 0);
    CHECK(play_tts_response(client_fd, bus_reader) == 0);

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
