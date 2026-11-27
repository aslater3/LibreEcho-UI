/* Wake-azimuth histogram and localization (#wake-direction). */
#include "adapter/wake_direction.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define PI 3.14159265358979323846
static int failures;

#define CHECK(condition, ...) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

static double angle_error(double a, double b)
{
    double error = fmod(fabs(a - b), 360.0);

    return error > 180.0 ? 360.0 - error : error;
}

static void test_empty(void)
{
    struct le_wake_direction direction;

    le_wake_direction_init(&direction, 12);
    CHECK(le_wake_direction_azimuth_deg(&direction) == -1, "empty has azimuth");
    CHECK(le_wake_direction_azimuth_rad(&direction) < 0.0f, "empty has radians");
    CHECK(le_wake_direction_confidence(&direction) == 0.0f,
          "empty has confidence");
    CHECK(le_wake_direction_variance_deg2(&direction) == 0.0f,
          "empty has variance");
    CHECK(le_wake_direction_samples(&direction) == 0, "empty has samples");
    CHECK(le_wake_direction_histogram(&direction, NULL, NULL, 12) == 0,
          "empty histogram");
}

static void test_single_direction(void)
{
    struct le_wake_direction direction;
    float angles[LE_WAKE_DIRECTION_MAX_BINS];
    float likelihood[LE_WAKE_DIRECTION_MAX_BINS];
    unsigned int count, i;
    int azimuth;

    le_wake_direction_init(&direction, 12);
    for (i = 0; i < 20; ++i)
        CHECK(le_wake_direction_add(&direction, 90, 4.0f) == 0, "add failed");
    azimuth = le_wake_direction_azimuth_deg(&direction);
    CHECK(angle_error(azimuth, 90.0) <= 5.0,
          "azimuth %.1f for a 90 deg source", (double)azimuth);
    CHECK(fabs(le_wake_direction_azimuth_rad(&direction) - PI / 2.0) < 0.1,
          "radians do not match degrees");
    CHECK(le_wake_direction_confidence(&direction) > 0.9f,
          "confidence did not saturate");
    CHECK(le_wake_direction_variance_deg2(&direction) < 400.0f,
          "variance too wide");
    count = le_wake_direction_histogram(&direction, angles, likelihood,
                                        LE_WAKE_DIRECTION_MAX_BINS);
    CHECK(count == 12, "histogram bin count %u", count);
    {
        unsigned int peak = 0;

        for (i = 1; i < count; ++i)
            if (likelihood[i] > likelihood[peak])
                peak = i;
        CHECK(angle_error(angles[peak], 90.0) <= 15.0,
              "histogram peak at %.0f deg", (double)angles[peak]);
    }
}

static void test_weighted_shift_and_wrap(void)
{
    struct le_wake_direction direction;
    unsigned int i;
    int azimuth;

    le_wake_direction_init(&direction, 12);
    for (i = 0; i < 10; ++i)
        le_wake_direction_add(&direction, 180, 1.0f);
    for (i = 0; i < 30; ++i)
        le_wake_direction_add(&direction, 270, 4.0f);
    azimuth = le_wake_direction_azimuth_deg(&direction);
    CHECK(angle_error(azimuth, 270.0) <= 10.0,
          "azimuth %.1f did not follow the heavier side", (double)azimuth);

    le_wake_direction_reset(&direction);
    for (i = 0; i < 30; ++i)
        le_wake_direction_add(&direction, 350, 2.0f);
    for (i = 0; i < 30; ++i)
        le_wake_direction_add(&direction, 10, 2.0f);
    azimuth = le_wake_direction_azimuth_deg(&direction);
    CHECK(angle_error(azimuth, 0.0) <= 10.0,
          "wrap-around azimuth %.1f is not near 0", (double)azimuth);
}

static void test_zero_weight_and_bad_input(void)
{
    struct le_wake_direction direction;

    le_wake_direction_init(&direction, 12);
    CHECK(le_wake_direction_add(&direction, 90, 0.0f) == 0, "zero add failed");
    CHECK(le_wake_direction_samples(&direction) == 1, "zero sample not counted");
    CHECK(le_wake_direction_azimuth_deg(&direction) == -1,
          "zero-weight sample produced an azimuth");
    CHECK(le_wake_direction_add(NULL, 0, 1.0f) < 0, "NULL accepted");
    CHECK(le_wake_direction_add(&direction, 1000, 1.0f) < 0,
          "out-of-range azimuth accepted");
    CHECK(le_wake_direction_add(&direction, -90, 1.0f) == 0,
          "negative azimuth rejected");
    CHECK(le_wake_direction_azimuth_deg(&direction) >= 0,
          "negative azimuth not folded");

    le_wake_direction_init(&direction, 0);
    CHECK(direction.bins == 1, "zero bins not clamped");
    le_wake_direction_init(&direction, 999);
    CHECK(direction.bins == LE_WAKE_DIRECTION_MAX_BINS, "bins not capped");
}

int main(void)
{
    test_empty();
    test_single_direction();
    test_weighted_shift_and_wrap();
    test_zero_weight_and_bad_input();
    if (failures) {
        fprintf(stderr, "%d wake-direction test(s) failed\n", failures);
        return 1;
    }
    printf("test-wake-direction: ok\n");
    return 0;
}
