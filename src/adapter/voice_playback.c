#define _POSIX_C_SOURCE 200809L

#include "voice_playback.h"

#include <errno.h>
#include <string.h>
#include <time.h>

static void *worker(void *opaque)
{
    struct le_voice_playback *playback = opaque;

    pthread_mutex_lock(&playback->mutex);
    while (playback->running) {
        char text[LE_VOICE_REPLY_SEGMENT_MAX];
        int result;

        while (playback->running && playback->count == 0)
            pthread_cond_wait(&playback->ready, &playback->mutex);
        if (!playback->running)
            break;
        memcpy(text, playback->items[playback->read_index],
               sizeof(text));
        playback->read_index =
            (playback->read_index + 1) % LE_VOICE_PLAYBACK_QUEUE;
        --playback->count;
        playback->playing = 1;
        pthread_mutex_unlock(&playback->mutex);
        result = playback->play
            ? playback->play(playback->context, text) : -1;
        pthread_mutex_lock(&playback->mutex);
        playback->playing = 0;
        if (result != 0)
            playback->failed = 1;
        if (playback->count == 0)
            pthread_cond_broadcast(&playback->idle);
    }
    playback->playing = 0;
    pthread_cond_broadcast(&playback->idle);
    pthread_mutex_unlock(&playback->mutex);
    return NULL;
}

int le_voice_playback_start(struct le_voice_playback *playback,
                            le_voice_playback_fn play,
                            void *context)
{
    if (!playback || !play)
        return -1;
    memset(playback, 0, sizeof(*playback));
    if (pthread_mutex_init(&playback->mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&playback->ready, NULL) != 0) {
        pthread_mutex_destroy(&playback->mutex);
        return -1;
    }
    if (pthread_cond_init(&playback->idle, NULL) != 0) {
        pthread_cond_destroy(&playback->ready);
        pthread_mutex_destroy(&playback->mutex);
        return -1;
    }
    playback->play = play;
    playback->context = context;
    playback->running = 1;
    if (pthread_create(&playback->thread, NULL, worker, playback) != 0) {
        pthread_cond_destroy(&playback->idle);
        pthread_cond_destroy(&playback->ready);
        pthread_mutex_destroy(&playback->mutex);
        return -1;
    }
    return 0;
}

int le_voice_playback_enqueue(struct le_voice_playback *playback,
                              const char *text)
{
    size_t length;
    int result = 0;

    if (!playback || !text || !text[0])
        return -1;
    length = strlen(text);
    if (length >= LE_VOICE_REPLY_SEGMENT_MAX)
        return -1;
    pthread_mutex_lock(&playback->mutex);
    if (!playback->running ||
        playback->count >= LE_VOICE_PLAYBACK_QUEUE) {
        result = -1;
    } else {
        memcpy(playback->items[playback->write_index],
               text, length + 1);
        playback->write_index =
            (playback->write_index + 1) % LE_VOICE_PLAYBACK_QUEUE;
        ++playback->count;
        pthread_cond_signal(&playback->ready);
    }
    pthread_mutex_unlock(&playback->mutex);
    return result;
}

int le_voice_playback_begin_turn(struct le_voice_playback *playback)
{
    int result = -1;

    if (!playback)
        return -1;
    pthread_mutex_lock(&playback->mutex);
    if (playback->running && playback->count == 0 &&
        !playback->playing) {
        playback->failed = 0;
        result = 0;
    }
    pthread_mutex_unlock(&playback->mutex);
    return result;
}

static void deadline_after(struct timespec *deadline,
                           unsigned int timeout_ms)
{
    unsigned long nanoseconds;

    clock_gettime(CLOCK_REALTIME, deadline);
    deadline->tv_sec += (time_t)(timeout_ms / 1000U);
    nanoseconds = (unsigned long)deadline->tv_nsec +
        (unsigned long)(timeout_ms % 1000U) * 1000000UL;
    deadline->tv_sec += (time_t)(nanoseconds / 1000000000UL);
    deadline->tv_nsec = (long)(nanoseconds % 1000000000UL);
}

int le_voice_playback_wait_idle(struct le_voice_playback *playback,
                                unsigned int timeout_ms)
{
    struct timespec deadline;
    int result = 0;

    if (!playback)
        return -1;
    deadline_after(&deadline, timeout_ms);
    pthread_mutex_lock(&playback->mutex);
    while (playback->running &&
           (playback->count != 0 || playback->playing)) {
        int wait_result =
            pthread_cond_timedwait(&playback->idle,
                                   &playback->mutex, &deadline);

        if (wait_result == ETIMEDOUT) {
            result = -1;
            break;
        }
        if (wait_result != 0) {
            result = -1;
            break;
        }
    }
    if (playback->failed)
        result = -1;
    pthread_mutex_unlock(&playback->mutex);
    return result;
}

void le_voice_playback_stop(struct le_voice_playback *playback)
{
    if (!playback)
        return;
    pthread_mutex_lock(&playback->mutex);
    playback->running = 0;
    pthread_cond_broadcast(&playback->ready);
    pthread_mutex_unlock(&playback->mutex);
    pthread_join(playback->thread, NULL);
    pthread_cond_destroy(&playback->idle);
    pthread_cond_destroy(&playback->ready);
    pthread_mutex_destroy(&playback->mutex);
    memset(playback, 0, sizeof(*playback));
}
