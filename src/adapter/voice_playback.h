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
    le_voice_playback_fn play;
    void *context;
};

int le_voice_playback_start(struct le_voice_playback *playback,
                            le_voice_playback_fn play,
                            void *context);
int le_voice_playback_enqueue(struct le_voice_playback *playback,
                              const char *text);
int le_voice_playback_begin_turn(struct le_voice_playback *playback);
int le_voice_playback_wait_idle(struct le_voice_playback *playback,
                                unsigned int timeout_ms);
void le_voice_playback_stop(struct le_voice_playback *playback);

#endif
