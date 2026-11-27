#include "gain_ramp.h"

#include <limits.h>

#define GAIN_Q16 65536

void le_gain_ramp_init(struct le_gain_ramp *ramp, unsigned int ramp_ms,
                       unsigned int start_hundredths,
                       unsigned int target_hundredths, unsigned int rate)
{
    unsigned long long total;

    if (!ramp)
        return;
    ramp->target_hundredths = target_hundredths;
    ramp->start_hundredths = start_hundredths;
    ramp->elapsed_samples = 0;
    ramp->gain_q16 = (int64_t)start_hundredths * GAIN_Q16;
    ramp->step_q16 = 0;
    ramp->total_samples = 0;
    if (ramp_ms == 0 || rate == 0 || start_hundredths == target_hundredths) {
        ramp->gain_q16 = (int64_t)target_hundredths * GAIN_Q16;
        return;
    }
    total = (unsigned long long)rate * ramp_ms / 1000U;
    if (total == 0)
        total = 1;
    if (total > UINT_MAX)
        total = UINT_MAX;
    ramp->total_samples = (unsigned int)total;
    ramp->step_q16 =
        ((int64_t)target_hundredths - (int64_t)start_hundredths) * GAIN_Q16 /
        (int64_t)total;
}

void le_gain_ramp_process(struct le_gain_ramp *ramp, int16_t *samples,
                          size_t count)
{
    size_t i;

    if (!ramp || !samples)
        return;
    for (i = 0; i < count; ++i) {
        int64_t scaled;

        if (ramp->gain_q16 <= 0) {
            samples[i] = 0;
        } else {
            /*
             * gain_q16 / (100 * 65536) is the linear factor. 64-bit
             * arithmetic keeps a full-scale sample times the 16x gain
             * ceiling in range.
             */
            scaled = (int64_t)samples[i] * ramp->gain_q16 /
                     ((int64_t)100 * GAIN_Q16);
            if (scaled > INT16_MAX)
                scaled = INT16_MAX;
            else if (scaled < INT16_MIN)
                scaled = INT16_MIN;
            samples[i] = (int16_t)scaled;
        }
        if (ramp->elapsed_samples < ramp->total_samples) {
            ramp->gain_q16 += ramp->step_q16;
            ++ramp->elapsed_samples;
            if (ramp->elapsed_samples == ramp->total_samples)
                ramp->gain_q16 =
                    (int64_t)ramp->target_hundredths * GAIN_Q16;
        }
    }
}

unsigned int le_gain_ramp_gain(const struct le_gain_ramp *ramp)
{
    if (!ramp || ramp->gain_q16 <= 0)
        return 0;
    return (unsigned int)(ramp->gain_q16 / GAIN_Q16);
}
