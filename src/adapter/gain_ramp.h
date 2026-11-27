#ifndef LE_GAIN_RAMP_H
#define LE_GAIN_RAMP_H

#include <stddef.h>
#include <stdint.h>

/*
 * Linear capture-gain ramp.
 *
 * The stock ASP front end has a GainRampUp stage ("Enable Gain Ramp",
 * "Ramp Type", "Ramp Up Time Constant (ms)", "Reboot Ramp Type", "Reboot Ramp
 * Up Time Constant (ms)", "Linear Ramp Factor"). It starts a capture at a
 * lower gain and walks it up to the configured level instead of applying the
 * whole step at once. This implements the linear shape only: it starts at
 * start_hundredths and reaches target_hundredths over ramp_ms.
 *
 * Gain is in hundredths, the same unit as LE_MIC_DIGITAL_GAIN: 100 is unity.
 * ramp_ms 0 disables the ramp, so every sample uses the target immediately.
 */
struct le_gain_ramp {
    unsigned int target_hundredths;
    unsigned int start_hundredths;
    unsigned int total_samples;
    unsigned int elapsed_samples;
    int64_t gain_q16;
    int64_t step_q16;
};

void le_gain_ramp_init(struct le_gain_ramp *ramp, unsigned int ramp_ms,
                       unsigned int start_hundredths,
                       unsigned int target_hundredths, unsigned int rate);

/* Scale count mono samples by the current ramp gain, advancing the ramp. */
void le_gain_ramp_process(struct le_gain_ramp *ramp, int16_t *samples,
                          size_t count);

/* Current gain in hundredths, for status and tests. */
unsigned int le_gain_ramp_gain(const struct le_gain_ramp *ramp);

#endif
