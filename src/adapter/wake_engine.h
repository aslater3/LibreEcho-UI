#ifndef LIBREECHO_WAKE_ENGINE_H
#define LIBREECHO_WAKE_ENGINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct le_wake_engine;

struct le_wake_engine *le_wake_engine_create(const char *model_directory,
                                             unsigned int threads);

/*
 * Feed continuous mono S16_LE/16 kHz audio.  A new classifier score is
 * produced every 1280 samples (80 ms).  `new_score` is zero when this call
 * only accumulated history and one when `score` was updated.
 */
int le_wake_engine_feed(struct le_wake_engine *engine,
                        const int16_t *samples,
                        size_t count,
                        float *score,
                        int *new_score);

unsigned int le_wake_engine_last_inference_us(
    const struct le_wake_engine *engine);

void le_wake_engine_destroy(struct le_wake_engine *engine);

#ifdef __cplusplus
}
#endif

#endif
