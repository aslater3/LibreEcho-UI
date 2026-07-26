#ifndef LIBREECHO_WAKE_WORKER_H
#define LIBREECHO_WAKE_WORKER_H

#include <stddef.h>
#include <stdint.h>

struct le_wake_worker {
    void *implementation;
};

struct le_wake_observation {
    uint64_t detection_sample;
    float vad_score;
    int vad_active;
    int playback_active;
};

struct le_wake_event {
    uint64_t detection_sample;
    float score;
    float vad_score;
    int playback_active;
    /*
     * Events cross a nonblocking in-process pipe. Keep the message
     * self-contained rather than copying a process-local pointer.
     */
    char model_name[24];
};

struct le_wake_worker_metrics {
    uint64_t scores;
    uint64_t events;
    uint64_t dropped_blocks;
    float max_score;
    unsigned int max_inference_us;
    int failed;
};

typedef void (*le_wake_event_callback)(
    const struct le_wake_event *event, void *opaque);

int le_wake_worker_start(struct le_wake_worker *worker,
                         const char *model_directory,
                         unsigned int threads,
                         float accept_threshold,
                         le_wake_event_callback callback,
                         void *callback_opaque);

/*
 * Submit continuous post-AEC mono PCM. This call only copies into a bounded
 * queue and never waits for ONNX inference.
 */
int le_wake_worker_submit(
    struct le_wake_worker *worker,
    const int16_t *samples,
    size_t count,
    const struct le_wake_observation *observation);

int le_wake_worker_set_threshold(
    struct le_wake_worker *worker, float accept_threshold);

void le_wake_worker_stop(
    struct le_wake_worker *worker,
    struct le_wake_worker_metrics *metrics);

#endif
