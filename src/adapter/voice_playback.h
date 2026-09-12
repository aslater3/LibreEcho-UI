#ifndef LIBREECHO_VOICE_PLAYBACK_H
#define LIBREECHO_VOICE_PLAYBACK_H

#include "voice_reply.h"

#include <pthread.h>
#include <stddef.h>

#define LE_VOICE_PLAYBACK_QUEUE 8

typedef int (*le_voice_playback_fn)(void *context, const char *text);

struct le_voice_playback {
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    pthread_cond_t idle;
    pthread_t thread;
    char items[LE_VOICE_PLAYBACK_QUEUE][LE_VOICE_REPLY_SEGMENT_MAX];
    size_t read_index;
    size_t write_index;
    size_t count;
    int playing;
    int running;
    int failed;
    int cancelled;
    le_voice_playback_fn play;
    void *context;
};

int le_voice_playback_start(struct le_voice_playback *playback,
                            le_voice_playback_fn play,
                            void *context);
int le_voice_playback_enqueue(struct le_voice_playback *playback,
                              const char *text);
int le_voice_playback_begin_turn(struct le_voice_playback *playback);

/*
 * Per-turn cancellation, for a spoken "stop" or anything else that has to
 * silence the current reply without ending playback for good. The sentence in
 * flight is abandoned -- its play callback observes
 * le_voice_playback_turn_cancelled() and returns -- and anything still queued
 * for this turn is dropped. The worker thread and every locked structure stay
 * alive, so the next le_voice_playback_begin_turn() starts a fresh turn. This
 * is deliberately not le_voice_playback_stop(), which is the lifecycle
 * destructor used at shutdown and must not run on a live daemon.
 */
void le_voice_playback_cancel_turn(struct le_voice_playback *playback);

/*
 * True while a cancelled sentence is still being played, which is the window
 * in which the play callback can abort it; false once that sentence has
 * finished, so standalone speech issued between turns is not blocked by a
 * completed cancellation. A new turn clears the cancellation entirely.
 */
int le_voice_playback_turn_cancelled(struct le_voice_playback *playback);
int le_voice_playback_wait_idle(struct le_voice_playback *playback,
                                unsigned int timeout_ms);
void le_voice_playback_stop(struct le_voice_playback *playback);

#endif
