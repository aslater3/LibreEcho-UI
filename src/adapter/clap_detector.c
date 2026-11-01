#define _POSIX_C_SOURCE 200809L

#include "clap_detector.h"

#include <math.h>
#include <string.h>

#define LE_CLAP_ONSET_RATIO 16.0f
#define LE_CLAP_MIN_ENERGY 22500.0f
#define LE_CLAP_MIN_NOISE 4.0f
#define LE_CLAP_NOISE_FALL 0.10f
#define LE_CLAP_NOISE_RISE 0.01f
#define LE_CLAP_CREST_MIN 3.0f
#define LE_CLAP_FLATNESS_MIN 0.30f
#define LE_CLAP_DECAY_RATIO 0.25f
#define LE_CLAP_WATCH_FRAMES 4U
#define LE_CLAP_REFRACTORY_FRAMES 12U
#define LE_CLAP_DOUBLE_MIN_FRAMES 15U
#define LE_CLAP_DOUBLE_MAX_FRAMES 80U

/* Goertzel centres in Hz, spread across the speech band and above. */
static const float clap_band_hz[LE_CLAP_BANDS] = {
    500.0f, 800.0f, 1200.0f, 1800.0f, 2700.0f, 4000.0f, 5500.0f, 7000.0f
};

void le_clap_detector_init(struct le_clap_detector *detector)
{
    memset(detector, 0, sizeof(*detector));
    detector->noise_energy = LE_CLAP_MIN_NOISE;
}

void le_clap_detector_set_enabled(struct le_clap_detector *detector,
                                  int enabled)
{
    le_clap_detector_init(detector);
    detector->enabled = enabled ? 1 : 0;
}

/*
 * Spectral flatness (geometric mean over arithmetic mean of band powers) from
 * eight Goertzel filters. Near 1 for a broadband transient, near 0 for a few
 * strong partials. Only called for frames that already passed the energy
 * onset test, so silence and steady sound never pay for it.
 */
static float band_flatness(const int16_t *samples)
{
    float log_sum = 0.0f;
    float power_sum = 0.0f;
    unsigned int band;
    size_t n;

    for (band = 0; band < LE_CLAP_BANDS; ++band) {
        const float coefficient =
            2.0f * cosf(2.0f * 3.14159265f * clap_band_hz[band] /
                        (float)LE_CLAP_RATE);
        float s1 = 0.0f;
        float s2 = 0.0f;
        float power;

        for (n = 0; n < LE_CLAP_FRAME_SAMPLES; ++n) {
            const float s0 = (float)samples[n] + coefficient * s1 - s2;

            s2 = s1;
            s1 = s0;
        }
        power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
        if (power < 1.0f)
            power = 1.0f;
        log_sum += logf(power);
        power_sum += power;
    }
    return expf(log_sum / (float)LE_CLAP_BANDS) /
           (power_sum / (float)LE_CLAP_BANDS);
}

static void frame_features(const int16_t *samples, float *energy,
                           float *crest)
{
    float sum = 0.0f;
    float peak = 0.0f;
    size_t n;

    for (n = 0; n < LE_CLAP_FRAME_SAMPLES; ++n) {
        const float value = (float)samples[n];
        const float magnitude = value < 0.0f ? -value : value;

        sum += value * value;
        if (magnitude > peak)
            peak = magnitude;
    }
    *energy = sum / (float)LE_CLAP_FRAME_SAMPLES;
    *crest = *energy > 0.0f ? peak / sqrtf(*energy) : 0.0f;
}

static void emit(struct le_clap_event *event, uint64_t frame,
                 unsigned int claps)
{
    event->detection_sample = frame * LE_CLAP_FRAME_SAMPLES;
    event->claps = claps;
}

int le_clap_detector_process(struct le_clap_detector *detector,
                             const int16_t *samples,
                             struct le_clap_event *event)
{
    uint64_t frame;
    float energy;
    float crest;
    int accepted = 0;
    uint64_t accepted_frame = 0;

    if (!detector->enabled)
        return 0;
    frame = detector->frames++;
    frame_features(samples, &energy, &crest);
    if (detector->refractory_frames_left)
        --detector->refractory_frames_left;

    if (detector->watching) {
        if (energy > detector->watch_peak_energy) {
            detector->watch_peak_energy = energy;
            detector->watch_peak_frame = frame;
        } else if (energy <=
                   detector->watch_peak_energy * LE_CLAP_DECAY_RATIO) {
            accepted = 1;
            accepted_frame = detector->watch_peak_frame;
            detector->watching = 0;
        } else if (frame - detector->watch_peak_frame >=
                   LE_CLAP_WATCH_FRAMES) {
            detector->watching = 0;
        }
    } else if (!detector->refractory_frames_left &&
               energy >= detector->noise_energy * LE_CLAP_ONSET_RATIO &&
               energy >= LE_CLAP_MIN_ENERGY &&
               crest >= LE_CLAP_CREST_MIN &&
               band_flatness(samples) >= LE_CLAP_FLATNESS_MIN) {
        detector->watching = 1;
        detector->watch_peak_energy = energy;
        detector->watch_peak_frame = frame;
    }

    if (!detector->watching && !accepted) {
        const float rate = energy < detector->noise_energy
            ? LE_CLAP_NOISE_FALL : LE_CLAP_NOISE_RISE;

        detector->noise_energy += (energy - detector->noise_energy) * rate;
        if (detector->noise_energy < LE_CLAP_MIN_NOISE)
            detector->noise_energy = LE_CLAP_MIN_NOISE;
    }

    if (accepted) {
        detector->refractory_frames_left = LE_CLAP_REFRACTORY_FRAMES;
        ++detector->accepted_count;
        if (detector->pending_single) {
            const uint64_t gap = accepted_frame - detector->pending_frame;

            if (gap < LE_CLAP_DOUBLE_MIN_FRAMES) {
                /* Too close to the pending clap to be a second clap. */
            } else if (gap <= LE_CLAP_DOUBLE_MAX_FRAMES) {
                emit(event, detector->pending_frame, LE_CLAP_DOUBLE);
                detector->pending_single = 0;
                return 1;
            } else {
                emit(event, detector->pending_frame, LE_CLAP_SINGLE);
                detector->pending_frame = accepted_frame;
                return 1;
            }
        } else {
            detector->pending_single = 1;
            detector->pending_frame = accepted_frame;
        }
    }

    if (detector->pending_single &&
        frame - detector->pending_frame > LE_CLAP_DOUBLE_MAX_FRAMES) {
        emit(event, detector->pending_frame, LE_CLAP_SINGLE);
        detector->pending_single = 0;
        return 1;
    }
    return 0;
}
