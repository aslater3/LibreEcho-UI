#include "wake_direction.h"

#include <math.h>
#include <string.h>

#define HISTOGRAM_DECAY 0.995f
#define SMOOTHING_ALPHA 0.15f
#define WEIGHT_CAP 8.0f
#define SUPPORT_SAMPLES 4.0f
#define RAD_TO_DEG 57.29577951308232f

static unsigned int nearest_bin(const struct le_wake_direction *direction,
                                int azimuth_deg)
{
    long index;

    azimuth_deg %= 360;
    if (azimuth_deg < 0)
        azimuth_deg += 360;
    index = lroundf((float)azimuth_deg * (float)direction->bins / 360.0f);
    if (index >= (long)direction->bins)
        index -= (long)direction->bins;
    return (unsigned int)index;
}

void le_wake_direction_init(struct le_wake_direction *direction,
                            unsigned int bins)
{
    if (!direction)
        return;
    memset(direction, 0, sizeof(*direction));
    if (bins < 1)
        bins = 1;
    if (bins > LE_WAKE_DIRECTION_MAX_BINS)
        bins = LE_WAKE_DIRECTION_MAX_BINS;
    direction->bins = bins;
}

void le_wake_direction_reset(struct le_wake_direction *direction)
{
    unsigned int bins;

    if (!direction)
        return;
    bins = direction->bins ? direction->bins : 1;
    memset(direction, 0, sizeof(*direction));
    direction->bins = bins;
}

int le_wake_direction_add(struct le_wake_direction *direction,
                          int azimuth_deg, float weight)
{
    unsigned int bin, left, right, i;
    float radians;

    if (!direction || direction->bins == 0)
        return -1;
    if (azimuth_deg < -360 || azimuth_deg > 720)
        return -1;
    if (!(weight > 0.0f))
        weight = 0.0f;
    if (weight > WEIGHT_CAP)
        weight = WEIGHT_CAP;

    for (i = 0; i < direction->bins; ++i)
        direction->histogram[i] *= HISTOGRAM_DECAY;
    ++direction->samples;
    if (weight <= 0.0f)
        return 0;

    bin = nearest_bin(direction, azimuth_deg);
    left = (bin + direction->bins - 1) % direction->bins;
    right = (bin + 1) % direction->bins;
    direction->histogram[bin] += weight;
    direction->histogram[left] += weight * 0.5f;
    direction->histogram[right] += weight * 0.5f;

    /* An exponential circular smoother stands in for the stock Kalman
       smoother. It only moves on observations that carry contrast, so a
       quiet room does not drag the estimate. */
    radians = (float)azimuth_deg * 3.14159265358979323846f / 180.0f;
    direction->smooth_re += SMOOTHING_ALPHA *
                            (cosf(radians) - direction->smooth_re);
    direction->smooth_im += SMOOTHING_ALPHA *
                            (sinf(radians) - direction->smooth_im);
    return 0;
}

static float resultant(const struct le_wake_direction *direction,
                       float *mean_deg)
{
    float sum = 0.0f, re = 0.0f, im = 0.0f;
    unsigned int i;

    if (!direction)
        return 0.0f;
    for (i = 0; i < direction->bins; ++i) {
        float angle = (float)i * 360.0f / (float)direction->bins;
        float radians = angle * 3.14159265358979323846f / 180.0f;

        sum += direction->histogram[i];
        re += direction->histogram[i] * cosf(radians);
        im += direction->histogram[i] * sinf(radians);
    }
    if (sum <= 0.0f)
        return 0.0f;
    re /= sum;
    im /= sum;
    if (mean_deg) {
        float degrees = atan2f(im, re) * RAD_TO_DEG;

        if (degrees < 0.0f)
            degrees += 360.0f;
        *mean_deg = degrees;
    }
    return sqrtf(re * re + im * im);
}

int le_wake_direction_azimuth_deg(const struct le_wake_direction *direction)
{
    float mean;

    if (!direction || direction->samples == 0)
        return -1;
    if (resultant(direction, &mean) <= 0.0f)
        return -1;
    return (int)lroundf(mean) % 360;
}

float le_wake_direction_azimuth_rad(const struct le_wake_direction *direction)
{
    int degrees = le_wake_direction_azimuth_deg(direction);

    if (degrees < 0)
        return -1.0f;
    return (float)degrees * 3.14159265358979323846f / 180.0f;
}

float le_wake_direction_variance_deg2(const struct le_wake_direction *direction)
{
    float r = resultant(direction, NULL);

    if (r <= 0.0f)
        return 0.0f;
    /* Circular variance 1-r, expressed in degrees squared. */
    return (1.0f - r) * RAD_TO_DEG * RAD_TO_DEG;
}

float le_wake_direction_confidence(const struct le_wake_direction *direction)
{
    float r = resultant(direction, NULL);
    float support;

    if (r <= 0.0f || !direction || direction->samples == 0)
        return 0.0f;
    support = (float)direction->samples / SUPPORT_SAMPLES;
    if (support > 1.0f)
        support = 1.0f;
    return r * support;
}

unsigned long le_wake_direction_samples(
    const struct le_wake_direction *direction)
{
    return direction ? direction->samples : 0;
}

unsigned int le_wake_direction_histogram(
    const struct le_wake_direction *direction, float *angles_deg,
    float *likelihood, unsigned int max)
{
    float sum = 0.0f;
    unsigned int i, count;

    if (!direction || direction->samples == 0 || max == 0)
        return 0;
    for (i = 0; i < direction->bins; ++i)
        sum += direction->histogram[i];
    if (sum <= 0.0f)
        return 0;
    count = direction->bins < max ? direction->bins : max;
    for (i = 0; i < count; ++i) {
        if (angles_deg)
            angles_deg[i] = (float)i * 360.0f / (float)direction->bins;
        if (likelihood)
            likelihood[i] = direction->histogram[i] / sum;
    }
    return count;
}
