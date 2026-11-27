#ifndef LE_WAKE_DIRECTION_H
#define LE_WAKE_DIRECTION_H

#include <stdint.h>

#include "mic_beamformer.h"

/*
 * Azimuth histogram for where speech comes from.
 *
 * The stock ASP front end localises with AWD/AARLocalization and reports
 * awdAzRad, awdAzVar, awdAzHistAngle and awdAzHistLik, smoothed by a
 * KalmanSmoother. This keeps the same shape without a model: each hop adds a
 * small kernel around the currently selected beam, weighted by how far that
 * beam stands above its noise floor. The resulting histogram gives a circular
 * mean, variance and confidence, and is exposed through micd for diagnostics.
 *
 * The histogram is deliberately not a VAD. A steady directional noise with no
 * speech-band contrast contributes little because its selected score stays
 * near one.
 */
#define LE_WAKE_DIRECTION_MAX_BINS LE_BEAM_MAX_BEAMS

struct le_wake_direction {
    unsigned int bins;
    float histogram[LE_WAKE_DIRECTION_MAX_BINS];
    float smooth_re;
    float smooth_im;
    unsigned long samples;
};

/* Reset the histogram and smoother. bins is clamped to 1..MAX_BINS. */
void le_wake_direction_init(struct le_wake_direction *direction,
                            unsigned int bins);

void le_wake_direction_reset(struct le_wake_direction *direction);

/*
 * Add one observation at azimuth_deg with a non-negative weight. A weight of
 * zero is recorded as a sample but contributes nothing to the histogram.
 * Returns 0, or -1 when the context or azimuth is unusable.
 */
int le_wake_direction_add(struct le_wake_direction *direction,
                          int azimuth_deg, float weight);

/* Circular mean in degrees in [0, 360), or -1 when nothing is accumulated. */
int le_wake_direction_azimuth_deg(const struct le_wake_direction *direction);

/* Circular mean in radians, or -1.0f when nothing is accumulated. */
float le_wake_direction_azimuth_rad(const struct le_wake_direction *direction);

/* Circular variance in degrees squared, or 0 when nothing is accumulated. */
float le_wake_direction_variance_deg2(const struct le_wake_direction *direction);

/* Resultant length scaled by sample support, in [0, 1]. */
float le_wake_direction_confidence(const struct le_wake_direction *direction);

unsigned long le_wake_direction_samples(
    const struct le_wake_direction *direction);

/*
 * Copy up to max bins of the normalised histogram. angles_deg and likelihood
 * may be NULL; each receives the bin angle and its share of the histogram.
 * Returns the number of bins copied, or 0 when nothing is accumulated.
 */
unsigned int le_wake_direction_histogram(
    const struct le_wake_direction *direction, float *angles_deg,
    float *likelihood, unsigned int max);

#endif
