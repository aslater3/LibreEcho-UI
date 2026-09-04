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

/*
 * Backend selection.
 *
 * The engine was previously fixed at build time by -DLE_WAKE_ENGINE_ONNX. A
 * device can now carry more than one and pick at startup, so two engines can
 * be compared on the same hardware and the same audio. le_wake_engine_create()
 * keeps its signature: it builds whichever backend is currently selected, and
 * tests that supply their own le_wake_engine_* at link time are unaffected.
 */

/* Select by name. Returns 0, or -1 (leaving the selection unchanged) if the
 * name is NULL, empty, or not compiled into this build. */
int le_wake_engine_select(const char *name);

/* The backend le_wake_engine_create() will use. Never NULL. */
const char *le_wake_engine_selected(void);

/* Enumerate compiled-in backends; NULL once index is past the end. */
const char *le_wake_engine_backend_name(unsigned int index);

/*
 * A backend implementation. Registered statically at build time; `impl` is the
 * backend's own handle and is never interpreted by the dispatcher.
 */
struct le_wake_engine_backend {
    const char *name;
    void *(*create)(const char *model_directory, unsigned int threads);
    int (*feed)(void *impl, const int16_t *samples, size_t count,
                float *score, int *new_score);
    unsigned int (*last_inference_us)(const void *impl);
    void (*destroy)(void *impl);
};

#ifdef __cplusplus
}
#endif

#endif
