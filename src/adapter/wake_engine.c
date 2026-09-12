/*
 * Wake-engine dispatcher.
 *
 * Which engine runs used to be decided at build time by -DLE_WAKE_ENGINE_ONNX.
 * That made it impossible to compare two engines on the same device and the
 * same audio, which is exactly what evaluating a new one requires. Backends are
 * now registered in a table and chosen by name at startup.
 *
 * le_wake_engine_create() deliberately keeps its old signature. The worker
 * calls it unchanged, and the tests that supply their own le_wake_engine_*
 * implementation at link time (test_wake_decode.c) keep working, because they
 * simply do not link this file.
 */

#include "wake_engine.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef LE_WAKE_ENGINE_ONNX
extern const struct le_wake_engine_backend le_wake_engine_onnx_backend;
#endif
#ifdef LE_WAKE_ENGINE_STUB
extern const struct le_wake_engine_backend le_wake_engine_stub_backend;
#endif

static const struct le_wake_engine_backend *const backends[] = {
#ifdef LE_WAKE_ENGINE_ONNX
    &le_wake_engine_onnx_backend,
#endif
#ifdef LE_WAKE_ENGINE_STUB
    &le_wake_engine_stub_backend,
#endif
    NULL
};

/*
 * The first registered backend is the default, so a build that carries only
 * one behaves exactly as it did before this indirection existed.
 */
static const struct le_wake_engine_backend *selected;

struct le_wake_engine {
    const struct le_wake_engine_backend *backend;
    void *impl;
};

static const struct le_wake_engine_backend *current(void)
{
    if (!selected)
        selected = backends[0];
    return selected;
}

const char *le_wake_engine_backend_name(unsigned int index)
{
    unsigned int i;

    for (i = 0; backends[i]; i++) {
        if (i == index)
            return backends[i]->name;
    }
    return NULL;
}

const char *le_wake_engine_selected(void)
{
    const struct le_wake_engine_backend *backend = current();

    /* Never NULL, so callers can log it without a special case. */
    return backend ? backend->name : "none";
}

int le_wake_engine_select(const char *name)
{
    unsigned int i;

    if (!name || !*name)
        return -1;
    for (i = 0; backends[i]; i++) {
        if (!strcmp(backends[i]->name, name)) {
            selected = backends[i];
            return 0;
        }
    }
    return -1;
}

struct le_wake_engine *le_wake_engine_create(const char *model_directory,
                                             unsigned int threads)
{
    const struct le_wake_engine_backend *backend = current();
    struct le_wake_engine *engine;

    if (!backend)
        return NULL;
    engine = calloc(1, sizeof(*engine));
    if (!engine)
        return NULL;
    engine->backend = backend;
    engine->impl = backend->create(model_directory, threads);
    if (!engine->impl) {
        free(engine);
        return NULL;
    }
    return engine;
}

int le_wake_engine_feed(struct le_wake_engine *engine,
                        const int16_t *samples,
                        size_t count,
                        float *score,
                        int *new_score)
{
    if (!engine)
        return -1;
    return engine->backend->feed(engine->impl, samples, count,
                                 score, new_score);
}

unsigned int le_wake_engine_last_inference_us(
    const struct le_wake_engine *engine)
{
    if (!engine)
        return 0;
    return engine->backend->last_inference_us(engine->impl);
}

void le_wake_engine_destroy(struct le_wake_engine *engine)
{
    if (!engine)
        return;
    engine->backend->destroy(engine->impl);
    free(engine);
}

#ifdef LE_WAKE_ENGINE_STUB
/*
 * Build-host stub. Exists so the selection logic above can be tested where the
 * ONNX runtime is unavailable; it is never compiled into a device image.
 */
static void *stub_create(const char *model_directory, unsigned int threads)
{
    (void)model_directory;
    (void)threads;
    return calloc(1, 1);
}

static int stub_feed(void *impl, const int16_t *samples, size_t count,
                     float *score, int *new_score)
{
    (void)impl;
    (void)samples;
    (void)count;
    if (score)
        *score = 0.5f;
    if (new_score)
        *new_score = 1;
    return 0;
}

static unsigned int stub_last_inference_us(const void *impl)
{
    (void)impl;
    return 42u;
}

static void stub_destroy(void *impl)
{
    free(impl);
}

const struct le_wake_engine_backend le_wake_engine_stub_backend = {
    "stub", stub_create, stub_feed, stub_last_inference_us, stub_destroy
};
#endif
