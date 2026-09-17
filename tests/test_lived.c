#define _POSIX_C_SOURCE 200809L

/*
 * libreecho-lived end-to-end, on the host.
 *
 * A stand-in waked serves the same two subscriptions the real daemon does -
 * a sample-indexed post-AEC audio stream and wake_detected events - and the
 * test drives the real lived binary through its control socket.  What is proven
 * here is the wiring the unit tests cannot reach: the frame reader, the preroll
 * handoff at the wake sample, playback into the bus, the control protocol, and
 * fail-closed behaviour when there is no usable transport.
 */

#include "adapter/voice_stream.h"

#include <errno.h>
#include <fcntl.h>
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

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static char directory[96];
/* How long the stand-in waked streams before it raises the wake. */
static unsigned int fake_wake_delay_ms;

static void sleep_ms(unsigned int milliseconds)
{
    struct timespec pause;

    pause.tv_sec = (time_t)(milliseconds / 1000U);
    pause.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    nanosleep(&pause, NULL);
}

static int join_path(char *out, size_t size, const char *dir, const char *name)
{
    size_t dir_length = strlen(dir);
    size_t name_length = strlen(name);

    if (dir_length + 1 + name_length + 1 > size)
        return -1;
    memcpy(out, dir, dir_length);
    out[dir_length] = '/';
    memcpy(out + dir_length + 1, name, name_length);
    out[dir_length + 1 + name_length] = '\0';
    return 0;
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

/* --- stand-in waked ----------------------------------------------------- */

static void put_u16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void put_u64(unsigned char *p, uint64_t value)
{
    put_u32(p, (uint32_t)value);
    put_u32(p + 4, (uint32_t)(value >> 32));
}

static int fake_frame(int fd, uint64_t first_sample, const int16_t *samples,
                      size_t count)
{
    unsigned char packet[LE_VOICE_STREAM_HEADER_BYTES +
                         LE_VOICE_STREAM_MAX_SAMPLES * 2U];

    put_u32(packet, LE_VOICE_STREAM_MAGIC);
    put_u16(packet + 4, LE_VOICE_STREAM_VERSION);
    put_u16(packet + 6, 0);
    put_u64(packet + 8, first_sample);
    put_u32(packet + 16, (uint32_t)count);
    put_u32(packet + 20, 0);
    memcpy(packet + LE_VOICE_STREAM_HEADER_BYTES, samples,
           count * sizeof(int16_t));
    return write_all(fd, packet,
                     LE_VOICE_STREAM_HEADER_BYTES + count * sizeof(int16_t));
}

static int fake_send(int fd, const char *line)
{
    return write_all(fd, line, strlen(line));
}

static int fake_listener(const char *path)
{
    struct sockaddr_un address;
    int listener;

    unlink(path);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(address.sun_path))
        return -1;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, 4) < 0)
        return -1;
    return listener;
}

/*
 * Serve subscribe and stream_audio in that order (lived opens them in that
 * order), stream one second of post-AEC audio, then raise a wake at the newest
 * sample - which is what waked reports.
 */
static int run_fake_waked(const char *path)
{
    int listener = fake_listener(path);
    int wake_fd;
    int audio_fd;
    char request[512];
    int16_t frame[160];
    uint64_t sample = 0;
    int i;
    size_t j;

    if (listener < 0)
        return 1;

    wake_fd = accept(listener, NULL, NULL);
    if (wake_fd < 0 || read_line(wake_fd, request, sizeof(request)) < 0)
        return 1;
    if (!strstr(request, "\"subscribe\""))
        return 1;
    if (fake_send(wake_fd, "{\"v\":1,\"id\":1,\"ok\":true,"
                           "\"data\":{\"subscribed\":true}}\n") < 0)
        return 1;

    audio_fd = accept(listener, NULL, NULL);
    if (audio_fd < 0 || read_line(audio_fd, request, sizeof(request)) < 0)
        return 1;
    if (!strstr(request, "\"stream_audio\""))
        return 1;
    if (fake_send(audio_fd,
                  "{\"v\":1,\"id\":1,\"ok\":true,\"data\":"
                  "{\"streaming\":true,\"format\":\"pcm_s16_le\","
                  "\"sample_rate\":16000,\"channels\":1,"
                  "\"frame_header_bytes\":24,\"sample_indexed\":true}}\n") < 0)
        return 1;

    /*
     * A second of audio so the preroll window is populated before the wake:
     * this is the condition that decides whether the first word is clipped.
     */
    for (i = 0; i < 100; ++i) {
        for (j = 0; j < 160; ++j)
            frame[j] = (int16_t)((i + (int)j) % 2 ? 120 : -120);
        if (fake_frame(audio_fd, sample, frame, 160) < 0)
            return 1;
        sample += 160;
    }

    if (fake_wake_delay_ms)
        sleep_ms(fake_wake_delay_ms);
    {
        char event[256];
        int length = snprintf(event, sizeof(event),
                              "{\"v\":1,\"event\":\"wake_detected\",\"data\":"
                              "{\"detection_sample\":%llu,\"score\":0.99,"
                              "\"vad_score\":1.0,\"playback_active\":false,"
                              "\"model\":\"alexa_v0.1\"}}\n",
                              (unsigned long long)sample);

        if (length <= 0 || fake_send(wake_fd, event) < 0)
            return 1;
    }

    /* Keep streaming so the session can be driven past the wake. */
    for (;;) {
        for (j = 0; j < 160; ++j)
            frame[j] = (int16_t)((int)j % 2 ? 120 : -120);
        if (fake_frame(audio_fd, sample, frame, 160) < 0)
            return 0;
        sample += 160;
        sleep_ms(10);
    }
}

/* --- control client ----------------------------------------------------- */

static int control_call(const char *socket_path, const char *command,
                        const char *args, char *out, size_t size)
{
    struct sockaddr_un address;
    char request[512];
    char response[4096];
    int fd;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    snprintf(request, sizeof(request),
             "{\"v\":1,\"id\":1,\"cmd\":\"%s\",\"args\":%s}\n", command,
             args ? args : "{}");
    if (write_all(fd, request, strlen(request)) < 0 ||
        read_line(fd, response, sizeof(response)) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    if (out && size)
        snprintf(out, size, "%s", response);
    return 0;
}

static int wait_for_socket(const char *path)
{
    int i;

    for (i = 0; i < 150; ++i) {
        if (access(path, F_OK) == 0)
            return 0;
        sleep_ms(20);
    }
    return -1;
}

static int wait_for_status(const char *socket_path, const char *needle)
{
    char status[4096];
    int i;

    for (i = 0; i < 200; ++i) {
        if (control_call(socket_path, "status", "{}", status,
                         sizeof(status)) == 0 &&
            strstr(status, needle) != NULL)
            return 0;
        sleep_ms(50);
    }
    fprintf(stderr, "status never reported %s\n", needle);
    if (control_call(socket_path, "status", "{}", status, sizeof(status)) == 0)
        fprintf(stderr, "last status: %s\n", status);
    return -1;
}

/* Poll until `needle` appears and `avoid` (optional) does not. */
static int wait_for_absent(const char *socket_path, const char *avoid)
{
    char status[4096];
    int i;

    for (i = 0; i < 120; ++i) {
        if (control_call(socket_path, "status", "{}", status,
                         sizeof(status)) == 0 &&
            strstr(status, avoid) == NULL)
            return 0;
        sleep_ms(50);
    }
    return -1;
}

static pid_t start_waked(const char *socket_path)
{
    pid_t pid;

    pid = fork();
    if (pid == 0)
        _exit(run_fake_waked(socket_path));
    return pid;
}

static int test_real_transport_fails_closed(void)
{
    char wake_socket[160];
    char control_socket[160];
    char bus_path[160];
    char status[4096];
    pid_t waked;
    pid_t daemon;
    int wait_status = 0;

    CHECK(join_path(wake_socket, sizeof(wake_socket), directory,
                    "realtime-wakeword.sock") == 0);
    CHECK(join_path(control_socket, sizeof(control_socket), directory,
                    "realtime-live.sock") == 0);
    CHECK(join_path(bus_path, sizeof(bus_path), directory,
                    "realtime.pcm") == 0);
    CHECK(close(open(bus_path, O_WRONLY | O_CREAT | O_TRUNC, 0600)) == 0);

    waked = start_waked(wake_socket);
    CHECK(waked > 0);
    CHECK(wait_for_socket(wake_socket) == 0);

    daemon = fork();
    CHECK(daemon >= 0);
    if (daemon == 0) {
        execl("./build/libreecho-lived", "libreecho-lived", "--foreground",
              "--transport", "realtime", "--socket", control_socket,
              "--wake-socket", wake_socket, "--audio-bus", bus_path,
              (char *)NULL);
        _exit(127);
    }
    CHECK(wait_for_socket(control_socket) == 0);

    CHECK(control_call(control_socket, "status", "{}", status,
                       sizeof(status)) == 0);
    CHECK(strstr(status, "\"enabled\":false") != NULL);
    CHECK(control_call(control_socket, "set_enabled", "{\"enabled\":true}",
                       status, sizeof(status)) == 0);

    /*
     * The published transport cannot reach GPT-Live in this build, so a wake
     * must produce a bounded, speakable failure - never a silent fallback to
     * the mock, which would report a conversation that never happened.
     */
    CHECK(control_call(control_socket, "wake", "{\"detection_sample\":16000}",
                       status, sizeof(status)) == 0);
    CHECK(wait_for_status(control_socket, "\"last_end\":\"connect_failed\"") == 0);
    CHECK(control_call(control_socket, "status", "{}", status,
                       sizeof(status)) == 0);
    CHECK(strstr(status, "\"transport\":\"realtime\"") != NULL);
    /* The reason depends on credential state: no credentials is the
       actionable one, and a signed-in device gets the missing-transport
       one instead. */
    CHECK(strstr(status, "sign in") != NULL ||
          strstr(status, "WebSocket") != NULL);
    CHECK(strstr(status, "\"sessions_failed\":") != NULL);
    CHECK(strstr(status, "\"sessions_started\":0") == NULL);

    kill(daemon, SIGTERM);
    CHECK(waitpid(daemon, &wait_status, 0) == daemon);
    kill(waked, SIGKILL);
    (void)waitpid(waked, &wait_status, 0);
    unlink(control_socket);
    unlink(wake_socket);
    unlink(bus_path);
    return 0;
}

static int test_mock_transport_full_conversation(void)
{
    char wake_socket[160];
    char control_socket[160];
    char bus_path[160];
    char status[4096];
    char transcript[4096];
    pid_t waked;
    pid_t daemon;
    int wait_status = 0;
    struct stat bus;

    CHECK(join_path(wake_socket, sizeof(wake_socket), directory,
                    "mock-wakeword.sock") == 0);
    CHECK(join_path(control_socket, sizeof(control_socket), directory,
                    "mock-live.sock") == 0);
    CHECK(join_path(bus_path, sizeof(bus_path), directory, "mock.pcm") == 0);
    CHECK(close(open(bus_path, O_WRONLY | O_CREAT | O_TRUNC, 0600)) == 0);

    /* Delay the wake so the "nothing before a wake" window is observable. */
    fake_wake_delay_ms = 1500;
    waked = start_waked(wake_socket);
    fake_wake_delay_ms = 0;
    CHECK(waked > 0);
    CHECK(wait_for_socket(wake_socket) == 0);

    daemon = fork();
    CHECK(daemon >= 0);
    if (daemon == 0) {
        execl("./build/libreecho-lived", "libreecho-lived", "--foreground",
              "--enable", "--transport", "mock", "--mock-scenario", "session", "--socket",
              control_socket, "--wake-socket", wake_socket, "--audio-bus",
              bus_path, "--conversation-timeout-ms", "800", (char *)NULL);
        _exit(127);
    }
    CHECK(wait_for_socket(control_socket) == 0);

    CHECK(control_call(control_socket, "status", "{}", status,
                       sizeof(status)) == 0);
    CHECK(strstr(status, "\"enabled\":true") != NULL);
    CHECK(strstr(status, "\"transport\":\"mock\"") != NULL);
    CHECK(strstr(status, "\"sessions_started\":0") != NULL);
    CHECK(strstr(status, "\"wake_events\":0") != NULL);
    CHECK(control_call(control_socket, "tools", "{}", status,
                       sizeof(status)) == 0);
    CHECK(strstr(status, "\"timer.set\"") != NULL);
    CHECK(strstr(status, "shell.exec") == NULL);

    /* The wake event published by the stand-in waked must start a session. */
    CHECK(wait_for_status(control_socket, "\"sessions_started\":1") == 0);
    /* The ring must have been fed from the indexed stream. */
    CHECK(wait_for_absent(control_socket, "\"ring_samples\":0,") == 0);
    /* ... and the model turn must reach the playback bus. */
    CHECK(wait_for_absent(control_socket, "\"frames_written\":0,") == 0);
    /* Delegation must have run through the allow-list. */
    CHECK(wait_for_absent(control_socket, "\"delegations\":0,") == 0);
    CHECK(wait_for_absent(control_socket, "\"audio_input_ms\":0,") == 0);

    CHECK(control_call(control_socket, "status", "{}", status,
                       sizeof(status)) == 0);
    CHECK(!strstr(status, "\"audio_input_ms\":0,"));
    CHECK(strstr(status, "\"transport_metrics\":{\"scenario\":\"session\"")
          != NULL);

    /* The transcript holds the conversation, and only when asked for it. */
    CHECK(control_call(control_socket, "transcript", "{}", transcript,
                       sizeof(transcript)) == 0);
    CHECK(strstr(transcript, "turn the kitchen lights off") != NULL);
    CHECK(strstr(status, "kitchen") == NULL);

    /* Disabling the mode closes the session and re-arms the wake path. */
    CHECK(control_call(control_socket, "set_enabled", "{\"enabled\":false}",
                       status, sizeof(status)) == 0);
    CHECK(strstr(status, "\"enabled\":false") != NULL);
    CHECK(wait_for_status(control_socket, "\"state\":\"idle\"") == 0);
    CHECK(control_call(control_socket, "status", "{}", status,
                       sizeof(status)) == 0);
    CHECK(strstr(status, "\"mode\":\"inactive\"") != NULL);
    CHECK(strstr(status, "\"ring_samples\":0,") != NULL);

    /* A second wake while disabled must not open a session. */
    {
        char before[4096];
        char after[4096];

        CHECK(control_call(control_socket, "status", "{}", before,
                           sizeof(before)) == 0);
        CHECK(control_call(control_socket, "wake", "{}", status,
                           sizeof(status)) == 0);
        CHECK(control_call(control_socket, "status", "{}", after,
                           sizeof(after)) == 0);
        CHECK(strstr(after, "\"state\":\"idle\"") != NULL);
    }

    kill(daemon, SIGTERM);
    CHECK(waitpid(daemon, &wait_status, 0) == daemon);
    kill(waked, SIGKILL);
    (void)waitpid(waked, &wait_status, 0);

    /* Model audio really went to the playback bus. */
    CHECK(stat(bus_path, &bus) == 0);
    CHECK(bus.st_size > 0);

    unlink(control_socket);
    unlink(wake_socket);
    unlink(bus_path);
    return 0;
}

int main(void)
{
    int failures = 0;

    snprintf(directory, sizeof(directory), "/tmp/le-lived-%d", (int)getpid());
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) {
        fprintf(stderr, "cannot create %s\n", directory);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    if (access("./build/libreecho-lived", X_OK) != 0) {
        fprintf(stderr, "build libreecho-lived first\n");
        return 1;
    }
    failures += test_real_transport_fails_closed() != 0;
    failures += test_mock_transport_full_conversation() != 0;
    rmdir(directory);
    if (failures) {
        fprintf(stderr, "lived: FAILED\n");
        return 1;
    }
    printf("lived: ok\n");
    return 0;
}
