#define _POSIX_C_SOURCE 200809L

#include "adapter/voice_pipeline.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

struct result_state {
    pthread_mutex_t mutex;
    struct le_voice_pipeline *pipeline;
    char text[4096];
    struct le_voice_pipeline_turn first_turn;
    struct le_voice_pipeline_turn turn;
    int called;
    int follow_up_result;
};

static void transcript(void *context, const char *text,
                       const struct le_voice_pipeline_turn *turn)
{
    struct result_state *state = context;

    pthread_mutex_lock(&state->mutex);
    snprintf(state->text, sizeof(state->text), "%s", text);
    if (!state->called)
        state->first_turn = *turn;
    state->turn = *turn;
    ++state->called;
    if (state->called == 1 && state->pipeline)
        state->follow_up_result =
            le_voice_pipeline_request_follow_up(state->pipeline);
    pthread_mutex_unlock(&state->mutex);
}

int main(void)
{
    char directory[] = "/tmp/libreecho-voice-pipeline-XXXXXX";
    char wake_socket[256];
    char stt_socket[256];
    struct le_voice_pipeline *pipeline = NULL;
    struct le_voice_pipeline_metrics metrics;
    struct result_state state;
    struct timespec delay = {0, 10000000L};
    pid_t stt_child = -1;
    pid_t source_child = -1;
    size_t attempt;
    int result = 0;

    memset(&state, 0, sizeof(state));
    signal(SIGPIPE, SIG_IGN);
    CHECK(setenv("LIBREECHO_STTD_FRAGMENT_TRANSCRIPT", "1", 1) == 0);
    CHECK(pthread_mutex_init(&state.mutex, NULL) == 0);
    CHECK(mkdtemp(directory) != NULL);
    snprintf(wake_socket, sizeof(wake_socket),
             "%s/wake.sock", directory);
    snprintf(stt_socket, sizeof(stt_socket),
             "%s/stt.sock", directory);
    stt_child = fork();
    CHECK(stt_child >= 0);
    if (stt_child == 0) {
        execl("./build/libreecho-sttd", "./build/libreecho-sttd",
              "--socket", stt_socket, "--model-dir", "mock",
              "--threads", "2", (char *)NULL);
        _exit(127);
    }
    for (attempt = 0;
         attempt < 300 && access(stt_socket, F_OK) != 0;
         ++attempt)
        nanosleep(&delay, NULL);
    CHECK(access(stt_socket, F_OK) == 0);
    source_child = fork();
    CHECK(source_child >= 0);
    if (source_child == 0) {
        execl("./build/mock-voice-source",
              "./build/mock-voice-source",
              wake_socket, (char *)NULL);
        _exit(127);
    }
    pipeline = le_voice_pipeline_start(
        wake_socket, stt_socket, transcript, &state);
    CHECK(pipeline != NULL);
    pthread_mutex_lock(&state.mutex);
    state.pipeline = pipeline;
    pthread_mutex_unlock(&state.mutex);
    for (attempt = 0; attempt < 500; ++attempt) {
        int called;

        pthread_mutex_lock(&state.mutex);
        called = state.called;
        pthread_mutex_unlock(&state.mutex);
        if (called >= 2)
            break;
        nanosleep(&delay, NULL);
    }
    {
        int called;
        int endpoint;
        uint64_t first_detection_sample;
        uint64_t follow_up_detection_sample;
        int follow_up_result;
        int first_follow_up;
        int second_follow_up;
        char text[sizeof(state.text)];

        pthread_mutex_lock(&state.mutex);
        called = state.called;
        endpoint = state.turn.endpoint;
        first_detection_sample = state.first_turn.detection_sample;
        follow_up_detection_sample = state.turn.detection_sample;
        follow_up_result = state.follow_up_result;
        first_follow_up = state.first_turn.follow_up;
        second_follow_up = state.turn.follow_up;
        snprintf(text, sizeof(text), "%s", state.text);
        pthread_mutex_unlock(&state.mutex);
        CHECK(called == 2);
        CHECK(follow_up_result == 0);
        CHECK(!strcmp(text, "mock transcription"));
        CHECK(first_detection_sample == 640);
        CHECK(follow_up_detection_sample > first_detection_sample);
        CHECK(!first_follow_up);
        CHECK(second_follow_up);
        CHECK(endpoint);
    }
    le_voice_pipeline_get_metrics(pipeline, &metrics);
    CHECK(metrics.wake_events == 1);
    CHECK(metrics.follow_up_listens == 1);
    CHECK(metrics.completed_transcripts == 2);
    CHECK(metrics.last_stt_audio_ms >= 200);
    puts("voice pipeline: indexed wake audio to streaming STT: ok");

cleanup:
    if (pipeline)
        le_voice_pipeline_stop(pipeline);
    if (source_child > 0) {
        kill(source_child, SIGTERM);
        waitpid(source_child, NULL, 0);
    }
    if (stt_child > 0) {
        kill(stt_child, SIGTERM);
        waitpid(stt_child, NULL, 0);
    }
    unlink(wake_socket);
    unlink(stt_socket);
    rmdir(directory);
    pthread_mutex_destroy(&state.mutex);
    return result;
}
