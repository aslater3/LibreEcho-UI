#define _POSIX_C_SOURCE 200809L

/*
 * #401: the local voice pipeline (agentd) sends speech recognition the mic
 * audio unchanged by default, and with noise suppression on it sends the
 * same stream with steady room noise taken down. Drives the production
 * capture worker against a mock wake socket and a mock streaming STT socket.
 */

#include "adapter/adapter.h"
#include "adapter/voice_pipeline.h"
#include "adapter/voice_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NOISE_SAMPLES 48000U
#define WAKE_SAMPLE 38000U
#define HEARD_SAMPLES 6400U

static int16_t noise[NOISE_SAMPLES];

static void make_noise(void)
{
    uint32_t state = 401U;
    size_t i;

    for (i = 0; i < NOISE_SAMPLES; ++i) {
        double sum = 0.0;
        int k;

        for (k = 0; k < 12; ++k) {
            state = state * 1664525U + 1013904223U;
            sum += (double)(state >> 8) / 16777216.0;
        }
        noise[i] = (int16_t)(800.0 * (sum - 6.0));
    }
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

static int accept_command(int listener, const char *expected,
                          const char *response)
{
    char request[LE_ADAPTER_MSG_MAX];
    char message[LE_ADAPTER_MSG_MAX];
    int client = le_adapter_accept(listener);
    int length;

    if (client < 0 ||
        read_line(client, request, sizeof(request)) < 0 ||
        !strstr(request, expected))
        return -1;
    length = le_adapter_respond_ok(message, sizeof(message), 1, response);
    if (length < 0 || write_all(client, message, (size_t)length) < 0) {
        close(client);
        return -1;
    }
    return client;
}

static int send_wake_event(int fd, uint64_t detection_sample)
{
    char event[LE_ADAPTER_MSG_MAX];
    char data[256];
    int length;

    snprintf(data, sizeof(data),
             "{\"detection_sample\":%llu,\"score\":0.8,\"vad_score\":1.0,"
             "\"playback_active\":false,\"model\":\"mock\"}",
             (unsigned long long)detection_sample);
    length = le_adapter_format_event(event, sizeof(event), "wake_detected",
                                     data);
    return length < 0 ? -1 : write_all(fd, event, (size_t)length);
}

static void millisleep(unsigned milliseconds)
{
    struct timespec delay;

    delay.tv_sec = (time_t)(milliseconds / 1000U);
    delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    nanosleep(&delay, NULL);
}

/* One LVS1 frame, written blocking (the production writer never blocks). */
static int write_frame(int fd, unsigned first)
{
    unsigned char packet[LE_VOICE_STREAM_HEADER_BYTES + 160U * 2U];
    unsigned i;

    memset(packet, 0, LE_VOICE_STREAM_HEADER_BYTES);
    for (i = 0; i < 4; ++i)
        packet[i] = (unsigned char)(LE_VOICE_STREAM_MAGIC >> (8U * i));
    packet[4] = LE_VOICE_STREAM_VERSION;
    for (i = 0; i < 4; ++i)
        packet[8 + i] = (unsigned char)(first >> (8U * i));
    packet[16] = 160U;
    for (i = 0; i < 160U; ++i) {
        uint16_t value = (uint16_t)noise[first + i];

        packet[LE_VOICE_STREAM_HEADER_BYTES + 2U * i] = (unsigned char)value;
        packet[LE_VOICE_STREAM_HEADER_BYTES + 2U * i + 1U] =
            (unsigned char)(value >> 8);
    }
    return write_all(fd, packet, sizeof(packet));
}

/* Streams the noise, wakes, and copies what recognition receives to out. */
static int mock_server(const char *wake_path, const char *stt_path, int out)
{
    unsigned char heard[HEARD_SAMPLES * 2U];
    size_t used = 0;
    unsigned first;
    int wake_listener, stt_listener, wake, audio, stt;

    signal(SIGPIPE, SIG_IGN);
    wake_listener = le_adapter_listen(wake_path);
    stt_listener = le_adapter_listen(stt_path);
    if (wake_listener < 0 || stt_listener < 0)
        return 1;
    wake = accept_command(wake_listener, "\"cmd\":\"subscribe\"",
                          "{\"subscribed\":true}");
    audio = accept_command(wake_listener, "\"cmd\":\"stream_audio\"",
                           "{\"streaming\":true,\"format\":\"pcm_s16_le\","
                           "\"sample_rate\":16000,\"channels\":1,"
                           "\"frame_header_bytes\":24,\"sample_indexed\":true}");
    if (wake < 0 || audio < 0)
        return 1;
    /* Accepted sockets are nonblocking; this mock may block on a full one. */
    if (fcntl(audio, F_SETFL, fcntl(audio, F_GETFL, 0) & ~O_NONBLOCK) < 0)
        return 1;
    for (first = 0; first < 40000U; first += 160U)
        if (write_frame(audio, first) < 0)
            return 1;
    millisleep(100);
    if (send_wake_event(wake, WAKE_SAMPLE) < 0)
        return 1;
    stt = accept_command(stt_listener, "\"cmd\":\"recognize_stream\"",
                         "{\"streaming\":true,\"sample_rate\":16000,"
                         "\"channels\":1,\"format\":\"pcm_s16_le\","
                         "\"max_seconds\":20}");
    if (stt < 0)
        return 1;
    for (; first < NOISE_SAMPLES; first += 160U)
        if (write_frame(audio, first) < 0)
            return 1;
    while (used < sizeof(heard)) {
        struct pollfd descriptor = {stt, POLLIN, 0};
        ssize_t count;

        if (poll(&descriptor, 1, 3000) != 1)
            return 1;
        count = read(stt, heard + used, sizeof(heard) - used);
        if (count <= 0)
            return 1;
        used += (size_t)count;
    }
    if (write_all(out, heard, sizeof(heard)) < 0)
        return 1;
    close(stt);
    close(wake);
    close(audio);
    return 0;
}

static double rms(const int16_t *samples, size_t count)
{
    double sum = 0.0;
    size_t i;

    for (i = 0; i < count; ++i)
        sum += (double)samples[i] * samples[i];
    return sqrt(sum / (double)count);
}

static void on_transcript(void *context, const char *text,
                          const struct le_voice_pipeline_turn *turn)
{
    (void)context;
    (void)text;
    (void)turn;
}

/* Returns 0 and fills heard, or -1. */
static int run_case(const char *directory, int suppress, int16_t *heard)
{
    char wake_path[256], stt_path[256];
    unsigned char bytes[HEARD_SAMPLES * 2U];
    struct le_voice_pipeline_metrics metrics;
    struct le_voice_pipeline *pipeline;
    size_t used = 0, i;
    int channel[2], status = 1;
    pid_t child;

    snprintf(wake_path, sizeof(wake_path), "%s/wake.sock", directory);
    snprintf(stt_path, sizeof(stt_path), "%s/stt.sock", directory);
    unlink(wake_path);
    unlink(stt_path);
    if (pipe(channel) < 0)
        return -1;
    child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        close(channel[0]);
        _exit(mock_server(wake_path, stt_path, channel[1]));
    }
    close(channel[1]);
    for (i = 0; i < 300 && (access(wake_path, F_OK) ||
                            access(stt_path, F_OK)); ++i)
        millisleep(10);
    pipeline = le_voice_pipeline_start(wake_path, stt_path, on_transcript,
                                       NULL);
    if (!pipeline) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        close(channel[0]);
        return -1;
    }
    if (suppress)
        le_voice_pipeline_set_noise_suppression(pipeline, 1);
    le_voice_pipeline_get_metrics(pipeline, &metrics);
    while (used < sizeof(bytes)) {
        struct pollfd descriptor = {channel[0], POLLIN, 0};
        ssize_t count;

        if (poll(&descriptor, 1, 8000) != 1)
            break;
        count = read(channel[0], bytes + used, sizeof(bytes) - used);
        if (count <= 0)
            break;
        used += (size_t)count;
    }
    close(channel[0]);
    waitpid(child, &status, 0);
    le_voice_pipeline_stop(pipeline);
    unlink(wake_path);
    unlink(stt_path);
    if (used != sizeof(bytes) || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0 ||
        metrics.noise_suppression != (suppress ? 1 : 0))
        return -1;
    for (i = 0; i < HEARD_SAMPLES; ++i)
        heard[i] = (int16_t)((unsigned)bytes[2 * i] |
                             ((unsigned)bytes[2 * i + 1] << 8));
    return 0;
}

int main(void)
{
    static int16_t heard[HEARD_SAMPLES];
    char directory[] = "/tmp/libreecho-pipeline-noise-XXXXXX";
    double before, after;

    signal(SIGPIPE, SIG_IGN);
    make_noise();
    if (!mkdtemp(directory)) {
        perror("mkdtemp");
        return 1;
    }
    if (run_case(directory, 0, heard) < 0) {
        fprintf(stderr, "default case did not reach recognition\n");
        rmdir(directory);
        return 1;
    }
    if (memcmp(heard, noise + WAKE_SAMPLE, sizeof(heard))) {
        fprintf(stderr, "default changed the audio sent to recognition\n");
        rmdir(directory);
        return 1;
    }
    if (run_case(directory, 1, heard) < 0) {
        fprintf(stderr, "suppressed case did not reach recognition\n");
        rmdir(directory);
        return 1;
    }
    rmdir(directory);
    before = rms(noise + WAKE_SAMPLE, HEARD_SAMPLES);
    after = rms(heard, HEARD_SAMPLES);
    printf("voice pipeline noise suppression: %.1f -> %.1f RMS\n", before,
           after);
    if (!(after < before * 0.5) || !(after > before * 0.1)) {
        fprintf(stderr, "steady noise not suppressed within 6..20 dB\n");
        return 1;
    }
    puts("voice pipeline noise suppression: off unchanged, on suppressed: ok");
    return 0;
}
