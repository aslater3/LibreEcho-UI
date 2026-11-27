#include "baby_cry_detector.h"

#include <math.h>
#include <string.h>

#define CRY_MIN_NOISE 4.0f
#define CRY_ONSET_RATIO 4.0f
#define CRY_MIN_ENERGY 400.0f
#define CRY_VOICING_MIN 0.55f
#define CRY_HARMONIC_MIN 0.25f
#define CRY_REFRACTORY_BOUTS 2U

static float frame_energy(const int16_t *samples, size_t n)
{
    float sum = 0.0f;
    size_t i;

    for (i = 0; i < n; ++i) {
        float value = samples[i];

        sum += value * value;
    }
    return sum / (float)n;
}

/*
 * Strongest normalised autocorrelation in the infant pitch range, and the
 * lag it belongs to.  The mean is removed first so a DC offset or a low hum
 * does not dominate.
 */
static float best_voicing(const int16_t *samples, size_t n,
                          unsigned int *lag_out)
{
    float mean = 0.0f;
    float centred[LE_CRY_FRAME_SAMPLES];
    float best = 0.0f;
    size_t i;
    unsigned int lag;

    for (i = 0; i < n; ++i)
        mean += samples[i];
    mean /= (float)n;
    for (i = 0; i < n; ++i)
        centred[i] = (float)samples[i] - mean;

    for (lag = LE_CRY_MIN_LAG; lag <= LE_CRY_MAX_LAG && lag < n; ++lag) {
        float product = 0.0f, left = 0.0f, right = 0.0f;

        for (i = 0; i + lag < n; ++i) {
            product += centred[i] * centred[i + lag];
            left += centred[i] * centred[i];
            right += centred[i + lag] * centred[i + lag];
        }
        if (left > 0.0f && right > 0.0f) {
            float value = product / sqrtf(left * right);

            if (value > best) {
                best = value;
                *lag_out = lag;
            }
        }
    }
    return best;
}

/*
 * Energy at the fundamental and its second harmonic, as a fraction of the
 * frame energy.  A cry has both; a whistle or a steady tone at one band does
 * not.
 */
static float harmonic_share(const int16_t *samples, unsigned int lag)
{
    float f0 = (float)LE_CRY_RATE / (float)lag;
    float energy = frame_energy(samples, LE_CRY_FRAME_SAMPLES) *
                   (float)LE_CRY_FRAME_SAMPLES;
    /*
     * A Goertzel bin over n samples holds about (A n / 2)^2 for a sine of
     * amplitude A; (n / 2) turns that back into a share of the frame's
     * summed energy.
     */
    const float scale = (float)LE_CRY_FRAME_SAMPLES / 2.0f;
    float bins[3];
    unsigned int band;

    for (band = 0; band < 3; ++band) {
        /* The last bin is the low guard: an adult male pitch below the
           infant range must not be read as its lowest lag. */
        float hz = band < 2 ? f0 * (float)(band + 1) : 120.0f;
        float coefficient =
            2.0f * cosf(2.0f * 3.14159265f * hz / (float)LE_CRY_RATE);
        float s1 = 0.0f, s2 = 0.0f;
        size_t i;

        for (i = 0; i < LE_CRY_FRAME_SAMPLES; ++i) {
            float s0 = (float)samples[i] + coefficient * s1 - s2;

            s2 = s1;
            s1 = s0;
        }
        bins[band] = (s1 * s1 + s2 * s2 - coefficient * s1 * s2) / scale;
    }
    if (energy <= 0.0f)
        return 0.0f;
    if (bins[2] > energy * 0.25f)
        return 0.0f;
    return (bins[0] + bins[1]) / energy;
}

void le_baby_cry_detector_init(struct le_baby_cry_detector *detector)
{
    memset(detector, 0, sizeof(*detector));
    detector->noise_energy = CRY_MIN_NOISE;
}

void le_baby_cry_detector_set_enabled(
    struct le_baby_cry_detector *detector, int enabled)
{
    le_baby_cry_detector_init(detector);
    detector->enabled = enabled ? 1 : 0;
}

int le_baby_cry_detector_process(
    struct le_baby_cry_detector *detector,
    const int16_t *samples,
    struct le_baby_cry_event *event)
{
    float energy = frame_energy(samples, LE_CRY_FRAME_SAMPLES);
    unsigned int lag = 0;
    int voiced = 0;

    if (!detector->enabled)
        return 0;
    ++detector->frames;
    if (energy < detector->noise_energy ||
        detector->noise_energy <= CRY_MIN_NOISE)
        detector->noise_energy =
            energy > CRY_MIN_NOISE ? energy : CRY_MIN_NOISE;
    else
        detector->noise_energy =
            detector->noise_energy * 0.999f + energy * 0.001f;

    if (detector->refractory_frames_left > 0) {
        --detector->refractory_frames_left;
        detector->voiced_frames = 0;
        detector->quiet_frames = 0;
        detector->bout_counted = 0;
        return 0;
    }
    if (energy > detector->noise_energy * CRY_ONSET_RATIO &&
        energy > CRY_MIN_ENERGY) {
        float voicing = best_voicing(samples, LE_CRY_FRAME_SAMPLES, &lag);

        if (voicing >= CRY_VOICING_MIN &&
            harmonic_share(samples, lag) >= CRY_HARMONIC_MIN)
            voiced = 1;
    }
    if (!voiced) {
        /*
         * A cry has glottal gaps and pitch breaks; a single unvoiced frame
         * must not restart the bout.  A longer gap does.
         */
        if (detector->voiced_frames > 0) {
            if (++detector->voiced_gap >= 5U) {
                detector->voiced_frames = 0;
                detector->voiced_gap = 0;
            }
        }
        if (detector->bout_counted) {
            if (++detector->quiet_frames >= LE_CRY_QUIET_FRAMES) {
                detector->bout_counted = 0;
                detector->quiet_frames = 0;
            }
        }
        return 0;
    }
    detector->voiced_gap = 0;
    detector->quiet_frames = 0;
    ++detector->voiced_frames;
    if (detector->voiced_frames < LE_CRY_MIN_VOICED_FRAMES ||
        detector->bout_counted)
        return 0;

    /* One bout is complete. A second one close by makes it a cry. */
    detector->bout_counted = 1;
    if (detector->bouts == 0) {
        detector->bouts = 1;
        detector->first_bout_end = detector->frames;
    } else if (detector->frames - detector->first_bout_end <=
               LE_CRY_BOUT_WINDOW_FRAMES) {
        ++detector->bouts;
        if (detector->bouts >= CRY_REFRACTORY_BOUTS) {
            ++detector->accepted_count;
            if (event) {
                event->detection_sample =
                    (detector->first_bout_end -
                     LE_CRY_MIN_VOICED_FRAMES) *
                    LE_CRY_FRAME_SAMPLES;
                event->bouts = detector->bouts;
            }
            detector->bouts = 0;
            detector->refractory_frames_left = LE_CRY_REFRACTORY_FRAMES;
            return 1;
        }
    } else {
        detector->bouts = 1;
        detector->first_bout_end = detector->frames;
    }
    return 0;
}
