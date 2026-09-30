#define _POSIX_C_SOURCE 200809L

/*
 * Focused harness for the voice pipeline's pre-transcript outcome callback.
 *
 * It drives the production capture/dispatch workers against a mock wake socket
 * and a mock streaming STT socket, and checks that a recognition failure, a
 * superseded wake and a recognition deadline are reported exactly once while
 * the normal transcript path keeps producing its transcript with no failure
 * outcome.
 *
 * Compile with:
 *   cc -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror \
 *      -Isrc -Isrc/adapter tests/test_voice_pipeline_outcomes.c \
 *      src/adapter/voice_pipeline.c src/adapter/voice_stream.c \
 *      src/adapter/voice_listening_led.c src/adapter/adapter_client.c \
 *      src/adapter/adapter_server.c src/json.c src/log.c -lpthread \
 *      -o build/test-voice-pipeline-outcomes
 */

#include "adapter/adapter.h"
#include "adapter/voice_pipeline.h"
#include "adapter/voice_stream.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        result = 1; \
        goto cleanup; \
    } \
} while (0)

#define MAX_OUTCOMES 8

struct capture {
    pthread_mutex_t mutex;
    int transcripts;
    int outcomes;
    char text[4096];
    int statuses[MAX_OUTCOMES];
    uint64_t detections[MAX_OUTCOMES];
};

static void on_transcript(void *context, const char *text,
                          const struct le_voice_pipeline_turn *turn)
{
    struct capture *capture = context;

    (void)turn;
    pthread_mutex_lock(&capture->mutex);
    ++capture->transcripts;
    snprintf(capture->text, sizeof(capture->text), "%s", text);
    pthread_mutex_unlock(&capture->mutex);
}

static void on_outcome(void *context,
                       const struct le_voice_pipeline_outcome *outcome)
{
    struct capture *capture = context;

    pthread_mutex_lock(&capture->mutex);
    if (capture->outcomes < MAX_OUTCOMES) {
        capture->statuses[capture->outcomes] = outcome->status;
        capture->detections[capture->outcomes] = outcome->detection_sample;
    }
    ++capture->outcomes;
    pthread_mutex_unlock(&capture->mutex);
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

static int send_transcript(int fd, const char *text)
{
    char event[LE_ADAPTER_MSG_MAX];
    char data[512];
    int length;

    snprintf(data, sizeof(data),
             "{\"text\":\"%s\",\"final\":true,\"endpoint\":true,"
             "\"audio_ms\":200,\"processing_ms\":10,\"total_ms\":210}", text);
    length = le_adapter_format_event(event, sizeof(event), "transcript", data);
    return length < 0 ? -1 : write_all(fd, event, (size_t)length);
}

static void millisleep(unsigned milliseconds)
{
    struct timespec delay;

    delay.tv_sec = (time_t)(milliseconds / 1000U);
    delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    nanosleep(&delay, NULL);
}

/* mode: 'f' stt failure, 't' recognition timeout, 'c' superseded wake,
 * 'o' normal transcript. Returns the child exit code. */
static int mock_server(char mode, const char *wake_path,
                       const char *stt_path)
{
    int16_t speech[160];
    int wake_listener, stt_listener;
    int wake, audio, stt;
    unsigned frame;

    signal(SIGPIPE, SIG_IGN);
    memset(speech, 0, sizeof(speech));
    for (frame = 0; frame < 160; ++frame)
        speech[frame] = 1200;
    wake_listener = le_adapter_listen(wake_path);
    if (wake_listener < 0)
        return 1;
    stt_listener = le_adapter_listen(stt_path);
    if (stt_listener < 0)
        return 1;
    wake = accept_command(wake_listener, "\"cmd\":\"subscribe\"",
                          "{\"subscribed\":true}");
    audio = accept_command(wake_listener, "\"cmd\":\"stream_audio\"",
                           "{\"streaming\":true,\"format\":\"pcm_s16_le\","
                           "\"sample_rate\":16000,\"channels\":1,"
                           "\"frame_header_bytes\":24,\"sample_indexed\":true}");
    if (wake < 0 || audio < 0)
        return 1;
    for (frame = 0; frame < 10; ++frame)
        if (le_voice_stream_write_frame(
                audio, (uint64_t)frame * 160U, speech, 160, 0) < 0)
            return 1;
    if (send_wake_event(wake, 640) < 0)
        return 1;
    stt = accept_command(stt_listener, "\"cmd\":\"recognize_stream\"",
                         "{\"streaming\":true,\"sample_rate\":16000,"
                         "\"channels\":1,\"format\":\"pcm_s16_le\","
                         "\"max_seconds\":20}");
    if (stt < 0)
        return 1;
    if (mode == 'f') {
        /* The recogniser fails on its own while wake/audio stay connected. */
        close(stt);
        millisleep(1200);
    } else if (mode == 't') {
        millisleep(1500);
        close(stt);
    } else if (mode == 'c') {
        millisleep(150);
        if (send_wake_event(wake, 9999) < 0)
            return 1;
        millisleep(700);
        close(stt);
    } else {
        millisleep(150);
        if (send_transcript(stt, "hello") < 0)
            return 1;
        millisleep(100);
        close(stt);
    }
    close(wake);
    close(audio);
    close(wake_listener);
    close(stt_listener);
    unlink(wake_path);
    unlink(stt_path);
    return 0;
}

int main(void)
{
    char directory[] = "/tmp/libreecho-pipeline-outcomes-XXXXXX";
    char wake_path[256];
    char stt_path[256];
    int result = 0;
    unsigned i;
    size_t attempt;

    signal(SIGPIPE, SIG_IGN);
    CHECK(mkdtemp(directory) != NULL);
    snprintf(wake_path, sizeof(wake_path), "%s/wake.sock", directory);
    snprintf(stt_path, sizeof(stt_path), "%s/stt.sock", directory);

    {
        const struct {
            char mode;
            const char *timeout;
            int expect_failure_status;   /* 0 = expect no outcome */
            int expect_cancelled;
            int expect_timeout;
            int expect_transcript;
        } cases[] = {
            { 'o', NULL,   LE_VOICE_TURN_NONE, 0, 0, 1 },
            { 'f', NULL,   LE_VOICE_TURN_STT_FAILED, 0, 0, 0 },
            { 't', "300",  LE_VOICE_TURN_NONE, 0, 1, 0 },
            { 'c', "5000", LE_VOICE_TURN_NONE, 1, 0, 0 }
        };
        unsigned case_index;

        for (case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]);
             ++case_index) {
            struct capture capture;
            struct le_voice_pipeline_metrics metrics;
            struct le_voice_pipeline *pipeline = NULL;
            pid_t child;
            int saw_failure = 0, saw_cancelled = 0, saw_timeout = 0;
            int transcripts, outcomes;

            memset(&capture, 0, sizeof(capture));
            CHECK(pthread_mutex_init(&capture.mutex, NULL) == 0);
            unlink(wake_path);
            unlink(stt_path);
            if (cases[case_index].timeout)
                CHECK(setenv("LE_VOICE_STT_TIMEOUT_MS",
                             cases[case_index].timeout, 1) == 0);
            else
                CHECK(unsetenv("LE_VOICE_STT_TIMEOUT_MS") == 0);
            child = fork();
            CHECK(child >= 0);
            if (child == 0) {
                _exit(mock_server(cases[case_index].mode, wake_path, stt_path));
            }
            for (attempt = 0; attempt < 300 &&
                 (access(wake_path, F_OK) != 0 ||
                  access(stt_path, F_OK) != 0); ++attempt)
                millisleep(10);
            CHECK(access(wake_path, F_OK) == 0);
            CHECK(access(stt_path, F_OK) == 0);
            pipeline = le_voice_pipeline_start(
                wake_path, stt_path, on_transcript, &capture);
            CHECK(pipeline != NULL);
            le_voice_pipeline_set_outcome_callback(pipeline, on_outcome,
                                                   &capture);
            for (attempt = 0; attempt < 500; ++attempt) {
                pthread_mutex_lock(&capture.mutex);
                transcripts = capture.transcripts;
                outcomes = capture.outcomes;
                pthread_mutex_unlock(&capture.mutex);
                if (cases[case_index].expect_transcript && transcripts >= 1)
                    break;
                if (cases[case_index].mode == 'f' && outcomes >= 1)
                    break;
                if (cases[case_index].mode == 't' && outcomes >= 1)
                    break;
                if (cases[case_index].mode == 'c' && outcomes >= 1)
                    break;
                millisleep(10);
            }
            le_voice_pipeline_get_metrics(pipeline, &metrics);
            pthread_mutex_lock(&capture.mutex);
            transcripts = capture.transcripts;
            outcomes = capture.outcomes;
            for (i = 0; i < (unsigned)outcomes && i < MAX_OUTCOMES; ++i) {
                if (capture.statuses[i] == LE_VOICE_TURN_STT_FAILED)
                    saw_failure = 1;
                if (capture.statuses[i] == LE_VOICE_TURN_CANCELLED)
                    saw_cancelled = 1;
                if (capture.statuses[i] == LE_VOICE_TURN_TIMED_OUT)
                    saw_timeout = 1;
            }
            CHECK(capture.transcripts == cases[case_index].expect_transcript);
            if (cases[case_index].expect_failure_status != LE_VOICE_TURN_NONE)
                CHECK(saw_failure);
            CHECK(saw_cancelled == cases[case_index].expect_cancelled);
            CHECK(saw_timeout == cases[case_index].expect_timeout);
            if (cases[case_index].expect_transcript) {
                CHECK(!strcmp(capture.text, "hello"));
                CHECK(outcomes == 0);
                CHECK(metrics.completed_transcripts == 1);
            }
            if (cases[case_index].mode == 'c') {
                CHECK(capture.detections[0] == 9999);
                CHECK(metrics.cancellations == 1);
            }
            if (cases[case_index].mode == 'f')
                CHECK(metrics.stt_failures == 1);
            if (cases[case_index].mode == 't')
                CHECK(metrics.timeouts == 1);
            pthread_mutex_unlock(&capture.mutex);
            le_voice_pipeline_stop(pipeline);
            kill(child, SIGTERM);
            waitpid(child, NULL, 0);
            pthread_mutex_destroy(&capture.mutex);
        }
    }

    unlink(wake_path);
    unlink(stt_path);
    CHECK(rmdir(directory) == 0);
    puts("voice pipeline outcomes: failure, supersede, timeout and success: ok");
    return 0;

cleanup:
    unlink(wake_path);
    unlink(stt_path);
    if (rmdir(directory) != 0)
        result = 1;
    return result;
}
