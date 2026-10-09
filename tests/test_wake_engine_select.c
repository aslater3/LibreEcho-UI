/*
 * Runtime wake-engine selection.
 *
 * The engine used to be fixed at compile time by -DLE_WAKE_ENGINE_ONNX, which
 * made it impossible to compare two engines on the same device, or to ship a
 * second one without rebuilding. The dispatcher lets waked pick a backend by
 * name at startup while keeping le_wake_engine_create()'s signature, so the
 * worker and the tests that fake the engine at link time are unaffected.
 *
 * Built against a stub backend (LE_WAKE_ENGINE_STUB) so the selection logic is
 * testable on the build host, where the real ONNX runtime is not available.
 */

#include "adapter/wake_engine.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    struct le_wake_engine *engine;
    float score = -1.0f;
    int new_score = -1;
    int16_t samples[8] = { 0 };
    unsigned int i;
    int seen_stub = 0;

    /* The stub backend is compiled in and therefore enumerable. */
    for (i = 0; le_wake_engine_backend_name(i); i++) {
        if (!strcmp(le_wake_engine_backend_name(i), "stub"))
            seen_stub = 1;
    }
    assert(seen_stub);
    assert(i > 0);

    /* Something sensible is always selected, without selecting first. */
    assert(le_wake_engine_selected() != NULL);

    /* An unknown name is refused and must not change the selection. */
    {
        const char *before = le_wake_engine_selected();
        assert(le_wake_engine_select("no-such-engine") < 0);
        assert(!strcmp(le_wake_engine_selected(), before));
    }

    /* A NULL or empty name is refused rather than crashing. */
    assert(le_wake_engine_select(NULL) < 0);
    assert(le_wake_engine_select("") < 0);

    /* A known name is accepted and reported back. */
    assert(le_wake_engine_select("stub") == 0);
    assert(!strcmp(le_wake_engine_selected(), "stub"));

    /* create/feed/destroy dispatch to the selected backend. */
    engine = le_wake_engine_create("/nonexistent-model-dir", 1);
    assert(engine != NULL);
    assert(le_wake_engine_feed(engine, samples, 8, &score, &new_score) == 0);
    /* The stub reports a fixed score so we can prove the call reached it. */
    assert(new_score == 1);
    assert(score > 0.4f && score < 0.6f);
    assert(le_wake_engine_last_inference_us(engine) == 42u);
    le_wake_engine_destroy(engine);

    /* Destroying NULL is a no-op, matching the ONNX backend's contract. */
    le_wake_engine_destroy(NULL);

    return 0;
}
