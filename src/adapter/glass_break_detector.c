#include "glass_break_detector.h"

#include <math.h>
#include <string.h>

/*
 * Band centres. The high set spans the range most of a shatter's energy
 * lands in; the low pair watches for the low-frequency thump of a door or a
 * hand, which a break does not have.
 */
static const float glass_high_hz[LE_GLASS_HIGH_BANDS] = {
    2500.0f, 3500.0f, 4500.0f, 5500.0f, 6500.0f
};
static const float glass_low_hz[LE_GLASS_LOW_BANDS] = {300.0f, 700.0f};

#define GLASS_MIN_NOISE 1.0f
#define GLASS_ONSET_RATIO 10.0f
#define GLASS_MIN_ENERGY 400.0f
#define GLASS_HIGH_DOMINANCE 0.60f
#define GLASS_MIN_CREST 2.5f
#define GLASS_MIN_FLATNESS 0.12f
#define GLASS_MIN_BURST_FRAMES 2U
#define GLASS_MAX_BURST_FRAMES 50U

static float goertzel_power(const int16_t *samples, size_t n, float hz)
{
    const float coefficient =
        2.0f * cosf(2.0f * 3.14159265f * hz / (float)LE_GLASS_RATE);
    float s1 = 0.0f, s2 = 0.0f;
    size_t i;

    for (i = 0; i < n; ++i) {
        float s0 = (float)samples[i] + coefficient * s1 - s2;

        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coefficient * s1 * s2;
}

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

/* Peak over RMS; an impulsive burst scores high, steady sound near one. */
static float crest_factor(const int16_t *samples, size_t n, float energy)
{
    float peak = 0.0f;
    size_t i;

    for (i = 0; i < n; ++i) {
        float value = (float)(samples[i] < 0 ? -samples[i] : samples[i]);

        if (value > peak)
            peak = value;
    }
    return energy > 0.0f ? peak / sqrtf(energy) : 0.0f;
}

/*
 * Decides whether the frame looks like a shatter or tinkle burst.  Only
 * called for frames that already clear the energy onset test.
 */
static int burst_shape(const int16_t *samples, float energy)
{
    float high = 0.0f, low = 0.0f, log_sum = 0.0f, sum = 0.0f;
    float flatness;
    unsigned int band;

    for (band = 0; band < LE_GLASS_HIGH_BANDS; ++band) {
        float power = goertzel_power(samples, LE_GLASS_FRAME_SAMPLES,
                                     glass_high_hz[band]);

        high += power;
        log_sum += logf(power + 1.0f);
        sum += power;
    }
    for (band = 0; band < LE_GLASS_LOW_BANDS; ++band) {
        float power = goertzel_power(samples, LE_GLASS_FRAME_SAMPLES,
                                     glass_low_hz[band]);

        low += power;
        log_sum += logf(power + 1.0f);
        sum += power;
    }
    if (sum <= 0.0f)
        return 0;
    flatness = expf(log_sum / (float)(LE_GLASS_HIGH_BANDS +
                                       LE_GLASS_LOW_BANDS)) /
               (sum / (float)(LE_GLASS_HIGH_BANDS + LE_GLASS_LOW_BANDS));
    return high > (high + low) * GLASS_HIGH_DOMINANCE &&
           crest_factor(samples, LE_GLASS_FRAME_SAMPLES, energy) >
               GLASS_MIN_CREST &&
           flatness > GLASS_MIN_FLATNESS;
}

void le_glass_break_detector_init(struct le_glass_break_detector *detector)
{
    memset(detector, 0, sizeof(*detector));
    detector->noise_energy = GLASS_MIN_NOISE;
}

void le_glass_break_detector_set_enabled(
    struct le_glass_break_detector *detector, int enabled)
{
    le_glass_break_detector_init(detector);
    detector->enabled = enabled ? 1 : 0;
}

int le_glass_break_detector_process(
    struct le_glass_break_detector *detector,
    const int16_t *samples,
    struct le_glass_break_event *event)
{
    float energy = frame_energy(samples, LE_GLASS_FRAME_SAMPLES);

    if (!detector->enabled)
        return 0;
    ++detector->frames;
    /*
     * The floor falls at once and rises slowly, so a burst cannot raise it
     * before the next burst is judged.
     */
    if (energy < detector->noise_energy ||
        detector->noise_energy <= GLASS_MIN_NOISE)
        detector->noise_energy = energy > GLASS_MIN_NOISE
            ? energy : GLASS_MIN_NOISE;
    else
        detector->noise_energy =
            detector->noise_energy * 0.999f + energy * 0.001f;

    if (detector->refractory_frames_left > 0) {
        --detector->refractory_frames_left;
        detector->in_burst = 0;
        return 0;
    }
    if (!detector->in_burst) {
        if (energy > detector->noise_energy * GLASS_ONSET_RATIO &&
            energy > GLASS_MIN_ENERGY &&
            burst_shape(samples, energy)) {
            detector->in_burst = 1;
            detector->burst_frames = 1;
            detector->burst_start_frame = detector->frames;
        }
        return 0;
    }
    ++detector->burst_frames;
    if (energy > detector->noise_energy * 2.0f &&
        detector->burst_frames < GLASS_MAX_BURST_FRAMES)
        return 0;

    if (detector->burst_frames >= GLASS_MIN_BURST_FRAMES &&
        detector->burst_frames <= GLASS_MAX_BURST_FRAMES) {
        if (detector->bursts == 0) {
            detector->bursts = 1;
            detector->first_burst_frame = detector->burst_start_frame;
        } else if (detector->frames - detector->first_burst_frame <=
                   LE_GLASS_TINKLE_FRAMES) {
            ++detector->bursts;
            if (detector->bursts >= 2) {
                ++detector->accepted_count;
                if (event) {
                    event->detection_sample =
                        detector->first_burst_frame *
                        LE_GLASS_FRAME_SAMPLES;
                    event->bursts = detector->bursts;
                }
                detector->bursts = 0;
                detector->refractory_frames_left =
                    LE_GLASS_REFRACTORY_FRAMES;
            }
        } else {
            detector->bursts = 1;
            detector->first_burst_frame = detector->burst_start_frame;
        }
    }
    detector->in_burst = 0;
    return detector->refractory_frames_left ==
           LE_GLASS_REFRACTORY_FRAMES;
}
