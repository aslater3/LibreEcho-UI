#define _POSIX_C_SOURCE 200809L

#include "adapter/voice_playback.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static unsigned long long monotonic_ms(void)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (unsigned long long)now.tv_sec * 1000ULL +
           (unsigned long long)now.tv_nsec / 1000000ULL;
}

struct output {
    struct le_voice_playback *playback;
    char values[4][LE_VOICE_REPLY_SEGMENT_MAX];
    size_t count;
    int halted;              /* the callback observed the cancellation */
    unsigned long long halted_ms;
};

static int play(void *context, const char *text)
{
    struct output *output = context;
    struct timespec delay = {0, 1000000L};

    if (output->count >= 4)
        return -1;
    snprintf(output->values[output->count],
             sizeof(output->values[output->count]), "%s", text);
    ++output->count;
    nanosleep(&delay, NULL);
    return 0;
}

/*
 * Mirrors agentd's play_sentence: the callback waits for the audio service to
 * report idle, polling for up to 30 seconds, and must give up as soon as the
 * turn is cancelled. Nothing else here ends the sentence, so returning at all
 * proves the cancellation reached the callback.
 */
static int play_slow(void *context, const char *text)
{
    struct output *output = context;
    struct timespec delay = {0, 10000000L};
    unsigned int attempt;

    if (output->count >= 4)
        return -1;
    snprintf(output->values[output->count],
             sizeof(output->values[output->count]), "%s", text);
    ++output->count;
    for (attempt = 0; attempt < 3000; ++attempt) {
        if (output->playback &&
            le_voice_playback_turn_cancelled(output->playback)) {
            output->halted = 1;
            output->halted_ms = monotonic_ms();
            return 0;
        }
        nanosleep(&delay, NULL);
    }
    return -1;
}

int main(void)
{
    struct timespec delay = {0, 10000000L};
    unsigned int i;

    /* Ordered asynchronous sentence queue. */
    {
        struct le_voice_playback playback;
        struct output output;

        memset(&output, 0, sizeof(output));
        CHECK(le_voice_playback_start(&playback, play, &output) == 0);
        CHECK(le_voice_playback_enqueue(&playback, "First sentence.") == 0);
        CHECK(le_voice_playback_enqueue(&playback, "Second sentence.") == 0);
        CHECK(le_voice_playback_wait_idle(&playback, 1000) == 0);
        CHECK(output.count == 2);
        CHECK(!strcmp(output.values[0], "First sentence."));
        CHECK(!strcmp(output.values[1], "Second sentence."));
        le_voice_playback_stop(&playback);
        puts("voice playback: ordered asynchronous sentence queue: ok");
    }

    /*
     * A spoken stop cancels the turn in flight. The callback must return
     * promptly, the sentences queued behind it must be dropped, and the
     * worker must stay alive: the turn after the stop begins and speaks
     * normally. The regression this pins is the stop path taking the
     * lifecycle destructor instead of a cancellation -- that joins the
     * worker before telling the audio service to stop (so the stop waits out
     * the callback's poll) and leaves every later begin_turn failing until
     * the daemon restarts.
     */
    {
        struct le_voice_playback playback;
        struct output output;
        unsigned long long cancel_ms;

        memset(&output, 0, sizeof(output));
        CHECK(le_voice_playback_start(&playback, play_slow, &output) == 0);
        output.playback = &playback;

        /* Begin turn, then speak: the worker is inside the callback. */
        CHECK(le_voice_playback_begin_turn(&playback) == 0);
        CHECK(le_voice_playback_enqueue(&playback, "First reply.") == 0);
        CHECK(le_voice_playback_enqueue(&playback, "Second reply.") == 0);
        for (i = 0; i < 200 && output.count < 1; ++i)
            nanosleep(&delay, NULL);
        CHECK(output.count == 1);
        pthread_mutex_lock(&playback.mutex);
        CHECK(playback.playing == 1);
        pthread_mutex_unlock(&playback.mutex);

        /* Stop transcript: cancel the turn. */
        cancel_ms = monotonic_ms();
        le_voice_playback_cancel_turn(&playback);
        CHECK(le_voice_playback_turn_cancelled(&playback) == 1);
        for (i = 0; i < 200 && !output.halted; ++i)
            nanosleep(&delay, NULL);
        CHECK(output.halted);
        CHECK(output.halted_ms - cancel_ms < 1000);
        CHECK(le_voice_playback_wait_idle(&playback, 2000) == 0);
        CHECK(output.count == 1);
        /* The cancelled sentence is over, so standalone speech made before the
           next turn would not be blocked by the completed cancellation. */
        CHECK(le_voice_playback_turn_cancelled(&playback) == 0);

        /* The worker survived: the next turn begins and speaks. */
        CHECK(le_voice_playback_begin_turn(&playback) == 0);
        CHECK(le_voice_playback_turn_cancelled(&playback) == 0);
        CHECK(le_voice_playback_enqueue(&playback, "Third reply.") == 0);
        for (i = 0; i < 200 && output.count < 2; ++i)
            nanosleep(&delay, NULL);
        CHECK(output.count == 2);
        CHECK(!strcmp(output.values[1], "Third reply."));

        le_voice_playback_cancel_turn(&playback);
        le_voice_playback_stop(&playback);
        puts("voice playback: a cancelled turn halts, drops its queue and "
             "keeps the worker: ok");
    }
    return 0;
}
