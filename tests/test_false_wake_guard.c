/*
 * Regression contract for the false-wake guard.
 *
 * The signals are synthetic: clipped overload, digital silence, a single
 * transient inside steady room noise, and steady speech-band noise. These
 * cases pin the decision rules and the score exemption. The thresholds
 * themselves await a loud room on real hardware.
 */

#include "adapter/false_wake_guard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW (LE_FW_FRAME_SAMPLES * LE_FW_WINDOW_FRAMES)

static int16_t clean[WINDOW];
static int16_t raw[LE_FW_FRAME_SAMPLES];
static uint32_t random_state = 777U;

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "false wake guard regression: %s\n", message);
        exit(1);
    }
}

static float random_unit(void)
{
    random_state = random_state * 1664525U + 1013904223U;
    return ((float)(random_state >> 8) / 8388608.0f) - 1.0f;
}

static void fill_noise(int amplitude)
{
    size_t n;

    for (n = 0; n < WINDOW; ++n)
        clean[n] = (int16_t)(random_unit() * (float)amplitude);
}

static void fill_raw(int value)
{
    size_t n;

    for (n = 0; n < LE_FW_FRAME_SAMPLES; ++n)
        raw[n] = (int16_t)value;
}

static int evaluate(struct le_false_wake_guard *guard, float score,
                    struct le_false_wake_result *result)
{
    return le_false_wake_guard_evaluate(guard, clean, WINDOW, score, result);
}

int main(void)
{
    struct le_false_wake_guard guard;
    struct le_false_wake_result result;
    unsigned int frame;

    /* Off: nothing is refused. */
    le_false_wake_guard_init(&guard);
    fill_noise(0);
    for (frame = 0; frame < LE_FW_HISTORY_FRAMES; ++frame)
        le_false_wake_guard_observe(&guard, raw, LE_FW_FRAME_SAMPLES);
    require_condition(!evaluate(&guard, 0.1f, &result),
                      "disabled guard refused an event");

    le_false_wake_guard_set_enabled(&guard, 1);

    /* Digital silence is not a wake word. */
    for (frame = 0; frame < LE_FW_HISTORY_FRAMES; ++frame) {
        fill_raw(0);
        le_false_wake_guard_observe(&guard, raw, LE_FW_FRAME_SAMPLES);
    }
    fill_noise(0);
    require_condition(evaluate(&guard, 0.99f, &result) && result.quiet,
                      "silence was accepted");

    /* Overload on either side of the canceller is refused. */
    for (frame = 0; frame < LE_FW_HISTORY_FRAMES; ++frame) {
        fill_raw(32767);
        le_false_wake_guard_observe(&guard, raw, LE_FW_FRAME_SAMPLES);
    }
    fill_noise(3000);
    require_condition(evaluate(&guard, 0.99f, &result) && result.clipped,
                      "clipped raw microphone was accepted");

    for (frame = 0; frame < LE_FW_HISTORY_FRAMES; ++frame) {
        fill_raw(1000);
        le_false_wake_guard_observe(&guard, raw, LE_FW_FRAME_SAMPLES);
    }
    for (frame = 0; frame < WINDOW; ++frame)
        clean[frame] = (int16_t)(random_unit() > 0.0f ? 32767 : -32768);
    require_condition(evaluate(&guard, 0.99f, &result) && result.clipped,
                      "clipped AFE output was accepted");

    /* Steady room noise is fine. */
    for (frame = 0; frame < LE_FW_HISTORY_FRAMES; ++frame) {
        fill_raw(200);
        le_false_wake_guard_observe(&guard, raw, LE_FW_FRAME_SAMPLES);
    }
    fill_noise(3000);
    require_condition(!evaluate(&guard, 0.60f, &result),
                      "steady noise was refused");
    require_condition(result.variance_ratio < LE_FW_VARIANCE_RATIO,
                      "steady noise looked like a transient");

    /* A transient far outside the window is refused unless the score is
       strong. */
    fill_noise(100);
    {
        size_t n;

        for (n = 0; n < LE_FW_FRAME_SAMPLES; ++n)
            clean[40 * LE_FW_FRAME_SAMPLES + n] =
                (int16_t)(random_unit() * 12000.0f);
    }
    require_condition(evaluate(&guard, 0.60f, &result) && result.wild,
                      "transient with a weak score was accepted");
    require_condition(!evaluate(&guard, 0.95f, &result),
                      "a strong score was refused by the variance rule");

    puts("false_wake_guard: clip, silence, transient, strong score ok");
    return 0;
}
