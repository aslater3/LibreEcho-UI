#include "gain_ramp.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition)                                                      \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,           \
                    #condition);                                              \
            return 1;                                                         \
        }                                                                     \
    } while (0)

/* The first sample keeps the start gain; the last reaches the target. */
static int ramp_reaches_target_after_its_time(void)
{
    struct le_gain_ramp ramp;
    int16_t samples[1600];
    unsigned int i;

    le_gain_ramp_init(&ramp, 100, 100, 400, 16000);
    CHECK(le_gain_ramp_gain(&ramp) == 100);
    memset(samples, 0, sizeof(samples));
    samples[0] = 1000;
    le_gain_ramp_process(&ramp, samples, 1);
    /* Unity on the first sample: the ramp starts at the baseline gain. */
    CHECK(samples[0] == 1000);
    le_gain_ramp_process(&ramp, samples + 1, 1599);
    CHECK(le_gain_ramp_gain(&ramp) == 400);
    samples[0] = 1000;
    le_gain_ramp_process(&ramp, samples, 1);
    CHECK(samples[0] == 4000);
    for (i = 0; i < 1600; ++i) {
        samples[i] = 1000;
    }
    le_gain_ramp_process(&ramp, samples, 1600);
    CHECK(samples[0] == 4000);
    CHECK(samples[1599] == 4000);
    return 0;
}

/* A disabled ramp applies the target on the very first sample. */
static int zero_ms_disables_the_ramp(void)
{
    struct le_gain_ramp ramp;
    int16_t samples[8];

    le_gain_ramp_init(&ramp, 0, 100, 400, 16000);
    CHECK(le_gain_ramp_gain(&ramp) == 400);
    memset(samples, 0, sizeof(samples));
    samples[0] = 1000;
    samples[1] = -1000;
    le_gain_ramp_process(&ramp, samples, 2);
    CHECK(samples[0] == 4000);
    CHECK(samples[1] == -4000);
    return 0;
}

/* The ramp is monotonic and each step is close to the linear slope. */
static int ramp_is_linear_and_monotonic(void)
{
    struct le_gain_ramp ramp;
    unsigned int previous;
    unsigned int i;

    le_gain_ramp_init(&ramp, 200, 100, 400, 16000);
    previous = le_gain_ramp_gain(&ramp);
    for (i = 1; i <= 3200; ++i) {
        int16_t sample = 1000;
        unsigned int gain;

        le_gain_ramp_process(&ramp, &sample, 1);
        gain = le_gain_ramp_gain(&ramp);
        CHECK(gain >= previous);
        if (gain - previous > 2) {
            fprintf(stderr, "ramp step %u: %u -> %u\n", i, previous, gain);
            return 1;
        }
        previous = gain;
    }
    CHECK(previous == 400);
    return 0;
}

/* A 16x digital gain and full-scale input clamp instead of wrapping. */
static int full_scale_clamps(void)
{
    struct le_gain_ramp ramp;
    int16_t samples[2] = {INT16_MAX, INT16_MIN};

    le_gain_ramp_init(&ramp, 0, 100, 1600, 16000);
    le_gain_ramp_process(&ramp, samples, 2);
    CHECK(samples[0] == INT16_MAX);
    CHECK(samples[1] == INT16_MIN);
    return 0;
}

int main(void)
{
    if (ramp_reaches_target_after_its_time())
        return 1;
    if (zero_ms_disables_the_ramp())
        return 1;
    if (ramp_is_linear_and_monotonic())
        return 1;
    if (full_scale_clamps())
        return 1;
    puts("gain ramp: starts at the baseline, reaches the target, clamps");
    return 0;
}
