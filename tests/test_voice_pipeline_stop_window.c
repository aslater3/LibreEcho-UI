#define _POSIX_C_SOURCE 200809L

/*
 * Stop without the wake word (#405): the wake-free stop window.
 *
 * Real voice pipeline, real mock sttd, and a scripted wake socket that streams
 * repeated short utterances. Checks that a window only opens while the
 * embedder asks for one, that its transcript reaches the stop callback and
 * never becomes a turn, that what it hears is discarded unless the callback
 * acts on it, and that the wake word still takes the recogniser.
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

/* Scripted audio: 300 ms of "speech", 400 ms of silence, repeated. */
#define CYCLES 5U
#define SPEECH_FRAMES 30U
#define SILENCE_FRAMES 40U
#define FRAME_SAMPLES 160U

struct observed {
    pthread_mutex_t mutex;
    int transcripts;
    int turn_begins;
    int stop_calls;
    int act;
    char transcript[256];
    char stop_text[256];
};

static void on_transcript(void *context, const char *text,
                          const struct le_voice_pipeline_turn *turn)
{
    struct observed *seen = context;

    (void)turn;
    pthread_mutex_lock(&seen->mutex);
    ++seen->transcripts;
    snprintf(seen->transcript, sizeof(seen->transcript), "%s", text);
    pthread_mutex_unlock(&seen->mutex);
}

static void on_turn_begin(void *context, uint64_t detection_sample)
{
    struct observed *seen = context;

    (void)detection_sample;
    pthread_mutex_lock(&seen->mutex);
    ++seen->turn_begins;
    pthread_mutex_unlock(&seen->mutex);
}

static int on_stop(void *context, const char *text)
{
    struct observed *seen = context;
    int act;

    pthread_mutex_lock(&seen->mutex);
    ++seen->stop_calls;
    snprintf(seen->stop_text, sizeof(seen->stop_text), "%s", text);
    act = seen->act;
    pthread_mutex_unlock(&seen->mutex);
    return act;
}

static int write_all(int fd, const char *text, size_t size)
{
    while (size) {
        ssize_t count = write(fd, text, size);

        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        text += count;
        size -= (size_t)count;
    }
    return 0;
}

static int accept_subscription(int listener, const char *data)
{
    char request[LE_ADAPTER_MSG_MAX], reply[LE_ADAPTER_MSG_MAX];
    size_t used = 0;
    int client = le_adapter_accept(listener), length;

    if (client < 0)
        return -1;
    while (used + 1 < sizeof(request)) {
        if (read(client, request + used, 1) != 1)
            return -1;
        if (request[used++] == '\n')
            break;
    }
    length = le_adapter_respond_ok(reply, sizeof(reply), 1, data);
    if (length < 0 || write_all(client, reply, (size_t)length) < 0)
        return -1;
    return client;
}

/* The wake socket: optionally announces one wake word mid-stream. */
static int run_source(const char *path, int wake_at_cycle)
{
    struct timespec frame_delay = {0, 10000000L};
    int16_t speech[FRAME_SAMPLES], silence[FRAME_SAMPLES];
    uint64_t sample = 0;
    unsigned cycle, frame;
    int listener, wake, audio;

    signal(SIGPIPE, SIG_IGN);
    for (frame = 0; frame < FRAME_SAMPLES; ++frame) {
        speech[frame] = 1200;
        silence[frame] = 0;
    }
    listener = le_adapter_listen(path);
    if (listener < 0)
        return 1;
    wake = accept_subscription(listener, "{\"subscribed\":true}");
    audio = accept_subscription(
        listener, "{\"streaming\":true,\"sample_indexed\":true}");
    if (wake < 0 || audio < 0)
        return 1;
    for (cycle = 0; cycle < CYCLES; ++cycle) {
        if ((int)cycle == wake_at_cycle) {
            char event[LE_ADAPTER_MSG_MAX], data[160];
            int length;

            snprintf(data, sizeof(data),
                     "{\"detection_sample\":%llu,\"score\":0.9}",
                     (unsigned long long)sample);
            length = le_adapter_format_event(event, sizeof(event),
                                             "wake_detected", data);
            if (length < 0 || write_all(wake, event, (size_t)length) < 0)
                return 1;
        }
        for (frame = 0; frame < SPEECH_FRAMES + SILENCE_FRAMES; ++frame) {
            if (le_voice_stream_write_frame(
                    audio, sample,
                    frame < SPEECH_FRAMES ? speech : silence,
                    FRAME_SAMPLES, 0) < 0)
                return 1;
            sample += FRAME_SAMPLES;
            nanosleep(&frame_delay, NULL);
        }
    }
    /* Keep the connections up until the parent tears the source down. */
    pause();
    return 0;
}

static pid_t spawn_sttd(const char *socket_path, const char *heard)
{
    pid_t child = fork();

    if (child == 0) {
        setenv("LIBREECHO_MOCK_STT_TEXT", heard, 1);
        execl("./build/libreecho-sttd", "./build/libreecho-sttd",
              "--socket", socket_path, "--model-dir", "mock",
              "--threads", "1", (char *)NULL);
        _exit(127);
    }
    return child;
}

static void wait_for(const char *path)
{
    struct timespec delay = {0, 10000000L};
    unsigned attempt;

    for (attempt = 0; attempt < 300 && access(path, F_OK) != 0; ++attempt)
        nanosleep(&delay, NULL);
}

static void reap(pid_t *child)
{
    if (*child > 0) {
        kill(*child, SIGTERM);
        waitpid(*child, NULL, 0);
    }
    *child = -1;
}

/*
 * One scenario. `listen` turns the stop window on before audio flows;
 * `act` is what the stop callback answers; `wake_at_cycle` (or -1) sends a
 * wake word at the start of that utterance.
 */
static int scenario(const char *name, const char *heard, int listen, int act,
                    int wake_at_cycle, struct observed *seen,
                    struct le_voice_pipeline_metrics *metrics)
{
    char directory[] = "/tmp/libreecho-stop-window-XXXXXX";
    char wake_socket[256], stt_socket[256];
    struct le_voice_pipeline *pipeline = NULL;
    struct timespec delay = {0, 50000000L};
    pid_t stt = -1, source = -1;
    unsigned waited;
    int result = 0;

    memset(seen, 0, sizeof(*seen));
    memset(metrics, 0, sizeof(*metrics));
    seen->act = act;
    CHECK(pthread_mutex_init(&seen->mutex, NULL) == 0);
    CHECK(mkdtemp(directory) != NULL);
    snprintf(wake_socket, sizeof(wake_socket), "%s/wake.sock", directory);
    snprintf(stt_socket, sizeof(stt_socket), "%s/stt.sock", directory);
    stt = spawn_sttd(stt_socket, heard);
    CHECK(stt > 0);
    wait_for(stt_socket);
    CHECK(access(stt_socket, F_OK) == 0);
    source = fork();
    CHECK(source >= 0);
    if (source == 0)
        _exit(run_source(wake_socket, wake_at_cycle));
    pipeline = le_voice_pipeline_start(wake_socket, stt_socket,
                                       on_transcript, seen);
    CHECK(pipeline != NULL);
    le_voice_pipeline_set_turn_begin_callback(pipeline, on_turn_begin, seen);
    le_voice_pipeline_set_stop_callback(pipeline, on_stop, seen);
    le_voice_pipeline_set_stop_listen(pipeline, listen);
    /* The scripted audio lasts 3.5 s; give the last utterance time to land. */
    for (waited = 0; waited < 90; ++waited)
        nanosleep(&delay, NULL);
    le_voice_pipeline_get_metrics(pipeline, metrics);
    le_voice_pipeline_set_stop_listen(pipeline, 0);
    for (waited = 0; waited < 10; ++waited) {
        struct le_voice_pipeline_metrics now;

        le_voice_pipeline_get_metrics(pipeline, &now);
        if (!now.stop_listening)
            break;
        nanosleep(&delay, NULL);
    }
    {
        struct le_voice_pipeline_metrics now;

        le_voice_pipeline_get_metrics(pipeline, &now);
        /* Turning listening off closes an open window. */
        CHECK(!now.stop_listening);
        CHECK(!now.stop_listen_requested);
    }
    printf("stop window: %s: windows=%lu heard=%lu stop_calls=%d "
           "transcripts=%d\n", name, metrics->stop_windows,
           metrics->stops_heard, seen->stop_calls, seen->transcripts);

cleanup:
    if (pipeline)
        le_voice_pipeline_stop(pipeline);
    reap(&source);
    reap(&stt);
    unlink(wake_socket);
    unlink(stt_socket);
    rmdir(directory);
    return result;
}

int main(void)
{
    struct observed seen;
    struct le_voice_pipeline_metrics metrics;
    int result = 0;

    signal(SIGPIPE, SIG_IGN);

    /* Off: nothing is transcribed without the wake word. */
    CHECK(scenario("off", "stop", 0, 1, -1, &seen, &metrics) == 0);
    CHECK(metrics.stop_windows == 0);
    CHECK(seen.stop_calls == 0);
    CHECK(seen.transcripts == 0);
    CHECK(seen.turn_begins == 0);

    /* On: a bare "stop" reaches the stop callback, and only there. */
    CHECK(scenario("stop", "stop", 1, 1, -1, &seen, &metrics) == 0);
    CHECK(metrics.stop_windows >= 1);
    CHECK(seen.stop_calls >= 1);
    CHECK(!strcmp(seen.stop_text, "stop"));
    CHECK(metrics.stops_heard == (unsigned long)seen.stop_calls);
    CHECK(seen.transcripts == 0);
    CHECK(seen.turn_begins == 0);
    CHECK(metrics.wake_events == 0);
    CHECK(metrics.completed_transcripts == 0);
    /* The cooldown after a heard stop means the next window waits, so five
       utterances cannot all be acted on. */
    CHECK(seen.stop_calls < (int)CYCLES);

    /* Room audio that is not a stop is offered, refused, and dropped: no
       turn, no history, nothing counted as heard. */
    CHECK(scenario("chatter", "tell me about the bus stop", 1, 0, -1,
                   &seen, &metrics) == 0);
    CHECK(seen.stop_calls >= 2);
    CHECK(metrics.stops_heard == 0);
    CHECK(seen.transcripts == 0);
    CHECK(seen.turn_begins == 0);
    CHECK(metrics.completed_transcripts == 0);
    CHECK(metrics.stt_failures == 0 && metrics.cancellations == 0);

    /* The wake word still wins: it closes the window and becomes a turn. */
    CHECK(scenario("wake", "what time is it", 1, 0, 2, &seen, &metrics) == 0);
    CHECK(metrics.wake_events == 1);
    CHECK(seen.turn_begins == 1);
    CHECK(seen.transcripts == 1);
    CHECK(!strcmp(seen.transcript, "what time is it"));
    CHECK(metrics.cancellations == 0);
    CHECK(metrics.dropped_turns == 0);
    CHECK(metrics.stop_windows >= 2);

    /* A window whose time runs out mid-word lets the word finish instead of
       discarding it. Each 300 ms utterance needs 500 ms to endpoint; with a
       150 ms window every one of them straddled a boundary and was lost. */
    setenv("LE_VOICE_STOP_WINDOW_MS", "150", 1);
    CHECK(scenario("boundary", "stop", 1, 1, -1, &seen, &metrics) == 0);
    unsetenv("LE_VOICE_STOP_WINDOW_MS");
    CHECK(seen.stop_calls >= 1);
    CHECK(!strcmp(seen.stop_text, "stop"));
    CHECK(metrics.stops_heard == (unsigned long)seen.stop_calls);
    CHECK(seen.transcripts == 0);
    CHECK(seen.turn_begins == 0);

    /* With no grace at all, a word cut off by the limit is still heard: the
       next window starts with the second of audio before it. */
    setenv("LE_VOICE_STOP_WINDOW_MS", "150", 1);
    setenv("LE_VOICE_STOP_GRACE_MS", "0", 1);
    CHECK(scenario("overlap", "stop", 1, 1, -1, &seen, &metrics) == 0);
    unsetenv("LE_VOICE_STOP_WINDOW_MS");
    unsetenv("LE_VOICE_STOP_GRACE_MS");
    CHECK(seen.stop_calls >= 1);
    CHECK(!strcmp(seen.stop_text, "stop"));
    CHECK(metrics.stops_heard == (unsigned long)seen.stop_calls);
    CHECK(seen.transcripts == 0);
    CHECK(seen.turn_begins == 0);
    /* The cooldown still holds: the replayed second never acts twice. */
    CHECK(seen.stop_calls < (int)CYCLES);

    puts("voice pipeline: stop without the wake word: ok");
cleanup:
    return result;
}
