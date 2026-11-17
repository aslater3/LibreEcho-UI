#define _POSIX_C_SOURCE 200809L

#include "smoke_alarm_detector.h"

#include <string.h>

#define LE_SMOKE_MIN_ENERGY 2500.0f
#define LE_SMOKE_ONSET_RATIO 8.0f
#define LE_SMOKE_MIN_NOISE 4.0f
#define LE_SMOKE_NOISE_FALL 0.10f
#define LE_SMOKE_NOISE_RISE 0.01f
#define LE_SMOKE_TONAL_MIN 0.25f
#define LE_SMOKE_PULSE_MIN_FRAMES 30U
#define LE_SMOKE_PULSE_MAX_FRAMES 90U
#define LE_SMOKE_GAP_MIN_FRAMES 30U
#define LE_SMOKE_GAP_MAX_FRAMES 90U
#define LE_SMOKE_COOLDOWN_FRAMES 1000U
#define LE_SMOKE_COUNT_CAP 100000U

/*
 * Goertzel coefficients 2 * cos(2 * pi * f / 16000) for 2.9, 3.1 and 3.3 kHz.
 * The 10 ms frame gives a 100 Hz bin, so the three centres cover the usual
 * 2.9-3.4 kHz smoke alarm band.
 */
static const float smoke_coefficients[3] = {
    0.837319f, 0.692234f, 0.542881f
};

void le_smoke_alarm_detector_init(struct le_smoke_alarm_detector *detector)
{
    memset(detector, 0, sizeof(*detector));
    detector->noise_energy = LE_SMOKE_MIN_NOISE;
}

void le_smoke_alarm_detector_set_enabled(
    struct le_smoke_alarm_detector *detector, int enabled)
{
    le_smoke_alarm_detector_init(detector);
    detector->enabled = enabled ? 1 : 0;
}

/*
 * Tonality of the strongest of the three filters: 1 for a pure tone at the
 * filter frequency, about 2 / N (0.0125) for white noise. Both energies are
 * taken from the same frame, so the result does not depend on the level.
 */
static float tonality(const int16_t *samples, float energy_sum)
{
    float best = 0.0f;
    unsigned int band;
    size_t n;

    if (energy_sum <= 0.0f)
        return 0.0f;
    for (band = 0; band < 3U; ++band) {
        const float coefficient = smoke_coefficients[band];
        float s1 = 0.0f;
        float s2 = 0.0f;
        float power;
        float value;

        for (n = 0; n < LE_SMOKE_FRAME_SAMPLES; ++n) {
            const float s0 = (float)samples[n] + coefficient * s1 - s2;

            s2 = s1;
            s1 = s0;
        }
        power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
        value = 2.0f * power /
                ((float)LE_SMOKE_FRAME_SAMPLES * energy_sum);
        if (value > best)
            best = value;
    }
    return best;
}

static void start_pulse(struct le_smoke_alarm_detector *detector,
                        uint64_t frame)
{
    /* A gap outside the T3 window breaks the sequence. */
    if (detector->pulses &&
        (detector->silent_frames < LE_SMOKE_GAP_MIN_FRAMES ||
         detector->silent_frames > LE_SMOKE_GAP_MAX_FRAMES))
        detector->pulses = 0;
    if (!detector->pulses)
        detector->first_pulse_frame = frame;
    detector->in_pulse = 1;
    detector->tone_frames = 0;
}

static int end_pulse(struct le_smoke_alarm_detector *detector,
                     struct le_smoke_alarm_event *event)
{
    detector->in_pulse = 0;
    detector->silent_frames = 1;
    if (detector->tone_frames < LE_SMOKE_PULSE_MIN_FRAMES ||
        detector->tone_frames > LE_SMOKE_PULSE_MAX_FRAMES) {
        detector->pulses = 0;
        return 0;
    }
    ++detector->pulses;
    if (detector->pulses < LE_SMOKE_PULSES_REQUIRED)
        return 0;
    event->detection_sample =
        detector->first_pulse_frame * LE_SMOKE_FRAME_SAMPLES;
    event->pulses = detector->pulses;
    detector->pulses = 0;
    detector->cooldown_frames_left = LE_SMOKE_COOLDOWN_FRAMES;
    return 1;
}

int le_smoke_alarm_detector_process(struct le_smoke_alarm_detector *detector,
                                    const int16_t *samples,
                                    struct le_smoke_alarm_event *event)
{
    uint64_t frame;
    float energy_sum = 0.0f;
    float energy;
    int tonal;
    size_t n;

    if (!detector->enabled)
        return 0;
    frame = detector->frames++;
    for (n = 0; n < LE_SMOKE_FRAME_SAMPLES; ++n)
        energy_sum += (float)samples[n] * (float)samples[n];
    energy = energy_sum / (float)LE_SMOKE_FRAME_SAMPLES;
    if (detector->cooldown_frames_left)
        --detector->cooldown_frames_left;

    tonal = !detector->cooldown_frames_left &&
            energy >= LE_SMOKE_MIN_ENERGY &&
            energy >= detector->noise_energy * LE_SMOKE_ONSET_RATIO &&
            tonality(samples, energy_sum) >= LE_SMOKE_TONAL_MIN;

    if (tonal) {
        if (!detector->in_pulse)
            start_pulse(detector, frame);
        if (detector->tone_frames < LE_SMOKE_COUNT_CAP)
            ++detector->tone_frames;
    } else {
        if (detector->in_pulse)
            return end_pulse(detector, event);
        if (detector->silent_frames < LE_SMOKE_COUNT_CAP)
            ++detector->silent_frames;
        if (detector->pulses &&
            detector->silent_frames > LE_SMOKE_GAP_MAX_FRAMES)
            detector->pulses = 0;
        const float rate = energy < detector->noise_energy
            ? LE_SMOKE_NOISE_FALL : LE_SMOKE_NOISE_RISE;

        detector->noise_energy += (energy - detector->noise_energy) * rate;
        if (detector->noise_energy < LE_SMOKE_MIN_NOISE)
            detector->noise_energy = LE_SMOKE_MIN_NOISE;
    }
    return 0;
}
