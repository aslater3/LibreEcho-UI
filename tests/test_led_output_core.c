/*
 * Focused behavioural tests for the common LED output stage (#66).
 *
 * Standalone harness.  Build:
 *   gcc -std=c99 -Wall -Wextra -Wpedantic -O2 -o build/test-led-output-core \
 *       tests/test_led_output_core.c src/adapter/led_output.c
 */
#include "../src/adapter/led_output.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "led_output: %s\n", message);
        exit(1);
    }
}

static unsigned int gamma_ref(unsigned int v)
{
    /* Independent reference for the physical gamma 2 curve: ceil(v^2 / 255). */
    if (v == 0U) return 0U;
    if (v >= 255U) return 255U;
    return (v * v + 254U) / 255U;
}

int main(void)
{
    const struct led_output_calibration *cal = led_output_default_calibration();
    struct led_output_calibration test_cal;
    struct led_output_state state;
    struct led_output_diag diag;
    struct led_rgb logical[LE_LED_PIXELS];
    struct led_rgb out[LE_LED_PIXELS];
    struct led_rgb raw[LE_LED_PIXELS];
    struct led_rgb warm, cool;
    unsigned int previous = 0U;
    int i;

    /*
     * 1. Transfer is monotonic, zero-preserving, non-boosting and
     *    non-vanishing: a physical gamma > 1 can only attenuate, so it can
     *    never brighten a dim or night/sleep-capped request.
     */
    require_condition(led_output_gamma(0) == 0U, "gamma(0) must be exactly 0");
    require_condition(led_output_gamma(255) == 255U, "gamma(255) must be 255");
    require_condition(led_output_gamma(1) == 1U,
                      "a nonzero request must keep at least one count");
    for (i = 1; i < 256; i++) {
        unsigned int g = led_output_gamma((unsigned int)i);
        require_condition(g >= previous, "gamma must be monotonic");
        require_condition(g <= (unsigned int)i,
                          "gamma must never boost low values: out <= in");
        require_condition(g == gamma_ref((unsigned int)i),
                          "gamma must be the deterministic integer curve");
        previous = g;
    }
    require_condition(led_output_gamma(64) < 64U,
                      "gamma must attenuate the low end, not expand it");
    require_condition(led_output_gamma(200) < 200U,
                      "a mid-level request must be reduced by the curve");

    /* 2. Exact off survives the whole pipeline. */
    memset(&state, 0, sizeof(state));
    for (i = 0; i < LE_LED_PIXELS; i++) logical[i] = (struct led_rgb){0, 0, 0};
    led_output_process(cal, &state, logical, 100U, out, &diag);
    for (i = 0; i < LE_LED_PIXELS; i++)
        require_condition(out[i].r == 0 && out[i].g == 0 && out[i].b == 0,
                          "all-zero frame must stay exactly off");
    require_condition(diag.frame_load == 0U && diag.raw_load == 0U,
                      "off frame must have zero load");

    /*
     * 3. With unity calibration and no budget, a full-brightness frame is
     *    exactly the transfer of the input: gamma is applied once.
     */
    test_cal = *cal;
    test_cal.max_frame_load = 0U;
    test_cal.max_slew_per_frame = 0U;
    memset(&state, 0, sizeof(state));
    for (i = 0; i < LE_LED_PIXELS; i++)
        logical[i] = (struct led_rgb){(unsigned int)(i * 20), 128U, 200U};
    led_output_process(&test_cal, &state, logical, 100U, out, &diag);
    for (i = 0; i < LE_LED_PIXELS; i++) {
        require_condition(out[i].r == led_output_gamma((unsigned int)(i * 20)),
                          "brightness 100 must apply the transfer exactly once");
        require_condition(out[i].g == led_output_gamma(128U) &&
                          out[i].b == led_output_gamma(200U),
                          "channels must be independently transferred");
    }

    /* 4. Brightness is the user cap: lowering it lowers output and load. */
    {
        unsigned int full_load, half_load;
        memset(&state, 0, sizeof(state));
        for (i = 0; i < LE_LED_PIXELS; i++)
            logical[i] = (struct led_rgb){200U, 200U, 200U};
        led_output_process(&test_cal, &state, logical, 100U, out, &diag);
        full_load = diag.frame_load;
        led_output_process(&test_cal, &state, logical, 50U, out, &diag);
        half_load = diag.frame_load;
        require_condition(half_load < full_load,
                          "brightness must reduce the frame load");
    }

    /* 5. A full-white frame is bounded by the reviewed budget, hue intact. */
    memset(&state, 0, sizeof(state));
    for (i = 0; i < LE_LED_PIXELS; i++)
        logical[i] = (struct led_rgb){255U, 255U, 255U};
    led_output_process(cal, &state, logical, 100U, out, &diag);
    require_condition(diag.limited == 1, "full white must hit the frame budget");
    require_condition(diag.frame_load <= cal->max_frame_load,
                      "budgeted load must not exceed the envelope");
    for (i = 1; i < LE_LED_PIXELS; i++)
        require_condition(out[i].r == out[0].r && out[i].g == out[0].g,
                          "budget scaling must be uniform across the ring");

    /*
     * 6. Proportional scaling preserves hue and spatial relationships: with
     *    two bright colours the ratio between the two pixels is unchanged.
     */
    memset(&state, 0, sizeof(state));
    for (i = 0; i < LE_LED_PIXELS; i++)
        logical[i] = (i % 2 == 0) ? (struct led_rgb){255U, 180U, 140U}
                                  : (struct led_rgb){200U, 240U, 255U};
    test_cal = *cal;
    test_cal.max_frame_load = 0U;      /* unlimited reference */
    led_output_process(&test_cal, &state, logical, 100U, raw, &diag);
    memset(&state, 0, sizeof(state));
    led_output_process(cal, &state, logical, 100U, out, &diag);
    require_condition(diag.limited == 1, "bright frame must be limited");
    for (i = 0; i < LE_LED_PIXELS; i++) {
        unsigned int ratio_raw = raw[i].r * 1000U / (raw[i].b + 1U);
        unsigned int ratio_out = out[i].r * 1000U / (out[i].b + 1U);
        unsigned int diff = ratio_raw > ratio_out ? ratio_raw - ratio_out
                                                  : ratio_out - ratio_raw;
        /* Integer rounding on small channels costs a few per-mille. */
        require_condition(diff * 100U <= ratio_raw * 8U,
                          "limiting must keep per-pixel hue relationships");
        require_condition(out[i].r <= raw[i].r && out[i].g <= raw[i].g &&
                          out[i].b <= raw[i].b,
                          "budget scaling must only attenuate");
    }

    /* 7. Named accents are restrained, distinct, and not raw white. */
    require_condition(led_output_accent(LE_ACCENT_WARM_WHITE, &warm) == 1,
                      "warm accent must exist");
    require_condition(led_output_accent(LE_ACCENT_COOL_WHITE, &cool) == 1,
                      "cool accent must exist");
    require_condition(!(warm.r == 255U && warm.g == 255U && warm.b == 255U),
                      "warm accent must not be raw white");
    require_condition(!(cool.r == 255U && cool.g == 255U && cool.b == 255U),
                      "cool accent must not be raw white");
    require_condition(warm.r != cool.r || warm.b != cool.b,
                      "warm and cool accents must differ");

    /* 8. Optional slew limiting bounds a sudden load increase. */
    test_cal = *cal;
    test_cal.max_slew_per_frame = 400U;
    memset(&state, 0, sizeof(state));
    for (i = 0; i < LE_LED_PIXELS; i++)
        logical[i] = (struct led_rgb){0U, 0U, 0U};
    led_output_process(&test_cal, &state, logical, 100U, out, &diag);
    for (i = 0; i < LE_LED_PIXELS; i++)
        logical[i] = (struct led_rgb){255U, 255U, 255U};
    led_output_process(&test_cal, &state, logical, 100U, out, &diag);
    require_condition(diag.slew_limited == 1,
                      "a sudden jump must be slew limited when enabled");
    require_condition(diag.frame_load <= 400U,
                      "slew-limited load must respect the step");
    /* A second identical frame is within the step and not slew-limited. */
    led_output_process(&test_cal, &state, logical, 100U, out, &diag);
    require_condition(diag.frame_load <= 800U,
                      "slew ceiling must rise with the previous accepted load");

    /*
     * 10. Safety ceiling: with unity calibration and no budget, no output
     *     channel may exceed the logical channel scaled by the requested
     *     brightness.  This is what makes the brightness, night and sleep caps
     *     true upper bounds -- a dim request can never be brightened by the
     *     curve -- and proves a nonzero cap stays visible (>= 1 count).
     */
    test_cal = *cal;
    test_cal.max_frame_load = 0U;
    test_cal.max_slew_per_frame = 0U;
    {
        static const unsigned int brightnesses[] = {100U, 50U, 20U, 8U, 4U, 1U};
        unsigned int b;
        for (b = 0U; b < sizeof(brightnesses) / sizeof(brightnesses[0]); b++) {
            unsigned int br = brightnesses[b];
            memset(&state, 0, sizeof(state));
            for (i = 0; i < LE_LED_PIXELS; i++)
                logical[i] = (struct led_rgb){255U, 214U, 170U};
            led_output_process(&test_cal, &state, logical, br, out, &diag);
            for (i = 0; i < LE_LED_PIXELS; i++) {
                unsigned int ceil_r = (255U * br + 50U) / 100U;
                unsigned int ceil_g = (214U * br + 50U) / 100U;
                unsigned int ceil_b = (170U * br + 50U) / 100U;
                require_condition(out[i].r <= ceil_r && out[i].g <= ceil_g &&
                                  out[i].b <= ceil_b,
                                  "output must not exceed the requested ceiling");
                require_condition(out[i].r >= 1U,
                                  "a nonzero cap must not vanish to black");
            }
        }
    }

    /* 9. Offline parity helper matches the inline transfer. */
    {
        unsigned char table[256];
        led_output_gamma_table(table);
        for (i = 0; i < 256; i++)
            require_condition(table[i] == (unsigned char)led_output_gamma((unsigned int)i),
                              "table must match the transfer");
    }

    puts("LED output stage: ok");
    return 0;
}
