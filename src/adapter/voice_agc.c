#include "voice_agc.h"

#include <math.h>

#define AGC_THRESHOLD 0.5f      /* -6 dBFS: peaks above this are compressed */
#define AGC_RATIO 2.0f
#define AGC_MAX_GAIN 2.0f       /* +6 dB on quiet speech */
#define AGC_MIN_GAIN 0.5f       /* -6 dB on loud speech */
#define AGC_GATE 0.0056f        /* about -45 dBFS: do not lift room noise */
#define AGC_ATTACK_MS 5.0f
#define AGC_RELEASE_MS 300.0f
#define AGC_GAIN_ATTACK_MS 20.0f
#define AGC_GAIN_RELEASE_MS 400.0f

static float time_coefficient(unsigned int rate, float milliseconds)
{
    float samples = (float)rate * milliseconds / 1000.0f;

    if (samples <= 1.0f)
        return 1.0f;
    return 1.0f - expf(-1.0f / samples);
}

int le_voice_agc_init(struct le_voice_agc *agc, unsigned int rate, int enabled)
{
    if (!agc || rate == 0)
        return -1;
    agc->rate = rate;
    agc->threshold = AGC_THRESHOLD;
    agc->ratio = AGC_RATIO;
    agc->max_gain = AGC_MAX_GAIN;
    agc->min_gain = AGC_MIN_GAIN;
    agc->gate = AGC_GATE;
    agc->attack_coeff = time_coefficient(rate, AGC_ATTACK_MS);
    agc->release_coeff = time_coefficient(rate, AGC_RELEASE_MS);
    agc->gain_attack_coeff = time_coefficient(rate, AGC_GAIN_ATTACK_MS);
    agc->gain_release_coeff = time_coefficient(rate, AGC_GAIN_RELEASE_MS);
    agc->envelope = 0.0f;
    agc->gain = 1.0f;
    agc->primed = enabled ? 1 : 0;
    return 0;
}

void le_voice_agc_process(struct le_voice_agc *agc, int16_t *samples,
                          size_t count)
{
    size_t i;

    if (!agc || !samples || !agc->primed)
        return;
    for (i = 0; i < count; ++i) {
        float x = (float)samples[i] / 32768.0f;
        float level = fabsf(x);
        float target, desired;
        float scaled;

        if (level > agc->envelope)
            agc->envelope += agc->attack_coeff * (level - agc->envelope);
        else
            agc->envelope += agc->release_coeff * (level - agc->envelope);
        if (agc->envelope < agc->gate) {
            /* Below the gate: hold unity so room noise is not pumped up. */
            desired = 1.0f;
        } else if (agc->envelope > agc->threshold) {
            /* Soft-knee-free compressor: bring the peak down to threshold. */
            target = powf(agc->threshold / agc->envelope,
                          1.0f - 1.0f / agc->ratio);
            desired = target;
        } else {
            /* Lift quiet speech toward the threshold, bounded by max gain. */
            target = agc->threshold / agc->envelope;
            desired = target > agc->max_gain ? agc->max_gain : target;
        }
        if (desired < agc->gain)
            agc->gain += agc->gain_attack_coeff * (desired - agc->gain);
        else
            agc->gain += agc->gain_release_coeff * (desired - agc->gain);
        if (agc->gain > agc->max_gain)
            agc->gain = agc->max_gain;
        else if (agc->gain < agc->min_gain)
            agc->gain = agc->min_gain;
        scaled = x * agc->gain * 32768.0f;
        if (scaled > INT16_MAX)
            scaled = INT16_MAX;
        else if (scaled < INT16_MIN)
            scaled = INT16_MIN;
        samples[i] = (int16_t)lrintf(scaled);
    }
}

float le_voice_agc_gain(const struct le_voice_agc *agc)
{
    return agc ? agc->gain : 1.0f;
}
