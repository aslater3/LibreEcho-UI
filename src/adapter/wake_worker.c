#define _POSIX_C_SOURCE 200809L

#include "wake_worker.h"

#include "wake_engine.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WAKE_BLOCK_SAMPLES 1280U
#define WAKE_QUEUE_BLOCKS 8U
#define WAKE_SUPPORT_THRESHOLD 0.35f
#define WAKE_LOCKOUT_SAMPLES 28000ULL

struct wake_block {
    int16_t samples[WAKE_BLOCK_SAMPLES];
    struct le_wake_observation observation;
};

struct wake_decoder {
    float scores[3];
    /*
     * The observation each score was computed from, carried alongside it so a
     * detection can be attributed to the frame that actually peaked rather
     * than to whichever frame happened to be newest when it was noticed.
     */
    struct le_wake_observation observations[3];
    unsigned int score_count;
    uint64_t lockout_until_sample;
};

struct wake_worker_impl {
    struct le_wake_engine *engine;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    struct wake_block queue[WAKE_QUEUE_BLOCKS];
    size_t read_position;
    size_t write_position;
    size_t queued;
    int stopping;
    int thread_started;

    int16_t accumulating[WAKE_BLOCK_SAMPLES];
    size_t accumulated;
    struct le_wake_observation accumulated_observation;

    struct wake_decoder decoder;
    struct le_wake_worker_metrics metrics;
    float accept_threshold;
    le_wake_event_callback callback;
    void *callback_opaque;
};

static void merge_observation(
    struct le_wake_observation *destination,
    const struct le_wake_observation *source)
{
    destination->detection_sample = source->detection_sample;
    if (source->vad_score > destination->vad_score)
        destination->vad_score = source->vad_score;
    if (source->vad_active)
        destination->vad_active = 1;
    if (source->playback_active)
        destination->playback_active = 1;
}

static void decode_score(struct wake_worker_impl *worker,
                         const struct wake_block *block,
                         float score)
{
    struct wake_decoder *decoder = &worker->decoder;
    float accept_threshold;
    unsigned int support = 0;
    unsigned int peak;
    unsigned int i;

    decoder->scores[0] = decoder->scores[1];
    decoder->scores[1] = decoder->scores[2];
    decoder->scores[2] = score;
    decoder->observations[0] = decoder->observations[1];
    decoder->observations[1] = decoder->observations[2];
    decoder->observations[2] = block->observation;
    if (decoder->score_count < 3)
        ++decoder->score_count;
    ++worker->metrics.scores;
    if (score > worker->metrics.max_score)
        worker->metrics.max_score = score;
    for (i = 3 - decoder->score_count; i < 3; ++i) {
        if (decoder->scores[i] >= WAKE_SUPPORT_THRESHOLD)
            ++support;
    }
    pthread_mutex_lock(&worker->mutex);
    accept_threshold = worker->accept_threshold;
    pthread_mutex_unlock(&worker->mutex);

    /*
     * Judge the window by its peak rather than by its newest frame.
     *
     * The support rule exists so that a single noisy frame cannot wake the
     * device, and that is worth keeping. But testing it against the newest
     * score alone made it unsatisfiable for any detection that rises quickly:
     * the two frames it looks back at are, by definition, the approach to the
     * peak and still low. Measured on hardware, an utterance scored
     * 0.083, 0.102, 0.554 across three consecutive frames -- the 0.554
     * cleared the 0.533 accept threshold, and was thrown away because the two
     * frames before it sat under the 0.35 support line. One frame later the
     * support was there and the score had already fallen to 0.414, under the
     * threshold. The window it needed never existed.
     *
     * Taking the peak of the three lets the corroboration come from either
     * side of it, which is what "two of the last three" was meant to mean. A
     * lone spike is still rejected, because a single frame over the support
     * line is still support of one. The cost is 80 ms of latency on the
     * detection, one frame, since the peak is confirmed only once the frame
     * after it has been scored.
     */
    peak = 0;
    for (i = 3 - decoder->score_count; i < 3; ++i) {
        if (decoder->scores[i] > decoder->scores[peak])
            peak = i;
    }
    if (decoder->observations[peak].detection_sample <
            decoder->lockout_until_sample ||
        !decoder->observations[peak].vad_active ||
        decoder->scores[peak] < accept_threshold || support < 2)
        return;

    ++worker->metrics.events;
    decoder->lockout_until_sample =
        decoder->observations[peak].detection_sample +
        WAKE_LOCKOUT_SAMPLES;
    if (worker->callback) {
        const struct le_wake_event event = {
            decoder->observations[peak].detection_sample,
            decoder->scores[peak],
            decoder->observations[peak].vad_score,
            decoder->observations[peak].playback_active,
            "alexa_v0.1"
        };

        worker->callback(&event, worker->callback_opaque);
    }
}

static void *wake_thread(void *opaque)
{
    struct wake_worker_impl *worker = opaque;

    for (;;) {
        struct wake_block block;
        float score = 0.0f;
        int new_score = 0;
        unsigned int inference_us;

        pthread_mutex_lock(&worker->mutex);
        while (worker->queued == 0 && !worker->stopping)
            pthread_cond_wait(&worker->condition, &worker->mutex);
        if (worker->queued == 0 && worker->stopping) {
            pthread_mutex_unlock(&worker->mutex);
            break;
        }
        block = worker->queue[worker->read_position];
        worker->read_position =
            (worker->read_position + 1U) % WAKE_QUEUE_BLOCKS;
        --worker->queued;
        pthread_mutex_unlock(&worker->mutex);

        if (le_wake_engine_feed(
                worker->engine, block.samples, WAKE_BLOCK_SAMPLES,
                &score, &new_score) < 0 || !new_score) {
            worker->metrics.failed = 1;
            break;
        }
        inference_us =
            le_wake_engine_last_inference_us(worker->engine);
        if (inference_us > worker->metrics.max_inference_us)
            worker->metrics.max_inference_us = inference_us;
        decode_score(worker, &block, score);
    }
    return NULL;
}

int le_wake_worker_start(struct le_wake_worker *worker,
                         const char *model_directory,
                         unsigned int threads,
                         float accept_threshold,
                         le_wake_event_callback callback,
                         void *callback_opaque)
{
    struct wake_worker_impl *impl;

    if (!worker || worker->implementation ||
        !model_directory || accept_threshold <= 0.0f ||
        accept_threshold >= 1.0f)
        return -1;
    impl = calloc(1, sizeof(*impl));
    if (!impl)
        return -1;
    impl->accept_threshold = accept_threshold;
    impl->callback = callback;
    impl->callback_opaque = callback_opaque;
    if (pthread_mutex_init(&impl->mutex, NULL) != 0)
        goto fail;
    if (pthread_cond_init(&impl->condition, NULL) != 0)
        goto fail_mutex;
    impl->engine = le_wake_engine_create(model_directory, threads);
    if (!impl->engine)
        goto fail_condition;
    if (pthread_create(&impl->thread, NULL, wake_thread, impl) != 0)
        goto fail_engine;
    impl->thread_started = 1;
    worker->implementation = impl;
    return 0;

fail_engine:
    le_wake_engine_destroy(impl->engine);
fail_condition:
    pthread_cond_destroy(&impl->condition);
fail_mutex:
    pthread_mutex_destroy(&impl->mutex);
fail:
    free(impl);
    return -1;
}

int le_wake_worker_submit(
    struct le_wake_worker *worker,
    const int16_t *samples,
    size_t count,
    const struct le_wake_observation *observation)
{
    struct wake_worker_impl *impl;
    size_t offset = 0;
    int dropped = 0;

    if (!worker || !worker->implementation ||
        !samples || !observation)
        return -1;
    impl = worker->implementation;
    while (offset < count) {
        size_t copying =
            WAKE_BLOCK_SAMPLES - impl->accumulated;

        if (copying > count - offset)
            copying = count - offset;
        memcpy(impl->accumulating + impl->accumulated,
               samples + offset, copying * sizeof(samples[0]));
        merge_observation(
            &impl->accumulated_observation, observation);
        impl->accumulated += copying;
        offset += copying;
        if (impl->accumulated == WAKE_BLOCK_SAMPLES) {
            pthread_mutex_lock(&impl->mutex);
            if (impl->queued == WAKE_QUEUE_BLOCKS) {
                ++impl->metrics.dropped_blocks;
                dropped = 1;
            } else {
                struct wake_block *block =
                    &impl->queue[impl->write_position];

                memcpy(block->samples, impl->accumulating,
                       sizeof(block->samples));
                block->observation =
                    impl->accumulated_observation;
                impl->write_position =
                    (impl->write_position + 1U) %
                    WAKE_QUEUE_BLOCKS;
                ++impl->queued;
                pthread_cond_signal(&impl->condition);
            }
            pthread_mutex_unlock(&impl->mutex);
            impl->accumulated = 0;
            memset(&impl->accumulated_observation, 0,
                   sizeof(impl->accumulated_observation));
        }
    }
    return dropped;
}

int le_wake_worker_set_threshold(
    struct le_wake_worker *worker, float accept_threshold)
{
    struct wake_worker_impl *impl;

    if (!worker || !worker->implementation ||
        accept_threshold <= 0.0f || accept_threshold >= 1.0f)
        return -1;
    impl = worker->implementation;
    pthread_mutex_lock(&impl->mutex);
    impl->accept_threshold = accept_threshold;
    pthread_mutex_unlock(&impl->mutex);
    return 0;
}

void le_wake_worker_stop(
    struct le_wake_worker *worker,
    struct le_wake_worker_metrics *metrics)
{
    struct wake_worker_impl *impl;

    if (metrics)
        memset(metrics, 0, sizeof(*metrics));
    if (!worker || !worker->implementation)
        return;
    impl = worker->implementation;
    pthread_mutex_lock(&impl->mutex);
    impl->stopping = 1;
    pthread_cond_signal(&impl->condition);
    pthread_mutex_unlock(&impl->mutex);
    if (impl->thread_started)
        pthread_join(impl->thread, NULL);
    if (metrics)
        *metrics = impl->metrics;
    le_wake_engine_destroy(impl->engine);
    pthread_cond_destroy(&impl->condition);
    pthread_mutex_destroy(&impl->mutex);
    free(impl);
    worker->implementation = NULL;
}
