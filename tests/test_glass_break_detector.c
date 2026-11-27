/*
 * Regression contract for the glass-break detector.
 *
 * The signals are synthetic: a break is a sharp high-frequency shatter
 * followed by a smaller tinkle; speech is a low-pitched harmonic voice;
 * a single burst is one clap. These cases pin the decision rules (two
 * high-band bursts inside the tinkle window, off by default). The hardware
 * run on real microphones is the evidence for the thresholds in a room.
 */

#include "adapter/glass_break_detector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE LE_GLASS_RATE
#define SECONDS 4
#define TOTAL (RATE * SECONDS)

static int16_t signal_buffer[TOTAL];
static uint32_t random_state = 4321U;

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "glass break detector regression: %s\n", message);
        exit(1);
    }
}

static float random_unit(void)
{
    random_state = random_state * 1664525U + 1013904223U;
    return ((float)(random_state >> 8) / 8388608.0f) - 1.0f;
}

static void fill_room_noise(float amplitude)
{
    size_t n;

    for (n = 0; n < TOTAL; ++n)
        signal_buffer[n] = (int16_t)(random_unit() * amplitude);
}

/*
 * A shatter is a few milliseconds of high-passed noise (the difference of
 * successive noise samples tilts the spectrum up) with a fast decay.  The
 * tinkle is the same shape, quieter and shorter.
 */
static void add_glass_burst(size_t start, float amplitude, float ms)
{
    size_t length = (size_t)(ms * (float)RATE / 1000.0f);
    float previous = 0.0f;
    float decay = 3.0f * (float)RATE / 1000.0f;
    size_t n;

    for (n = 0; n < length && start + n < TOTAL; ++n) {
        float fresh = random_unit();
        float high = fresh - previous;
        float value = signal_buffer[start + n] +
                      amplitude * high * expf(-(float)n / decay);

        previous = fresh;
        if (value > 32767.0f) value = 32767.0f;
        if (value < -32768.0f) value = -32768.0f;
        signal_buffer[start + n] = (int16_t)value;
    }
}

/* Speech-like: a 140 Hz harmonic voice with a slow envelope. */
static void add_voiced(size_t start, float amplitude, float ms)
{
    size_t length = (size_t)(ms * (float)RATE / 1000.0f);
    size_t n;

    for (n = 0; n < length && start + n < TOTAL; ++n) {
        float t = (float)(start + n) / (float)RATE;
        float envelope = expf(-(float)n / (float)(length / 2U));
        float voice = 0.0f;
        int harmonic;

        for (harmonic = 1; harmonic <= 20; ++harmonic)
            voice += sinf(2.0f * 3.14159265f * 140.0f * (float)harmonic * t) /
                     (float)harmonic;
        signal_buffer[start + n] += (int16_t)(voice * envelope * amplitude);
    }
}

static int count_events(const struct le_glass_break_detector *detector,
                        uint64_t *detection_sample)
{
    size_t offset;
    int count = 0;

    for (offset = 0; offset + LE_GLASS_FRAME_SAMPLES <= TOTAL;
         offset += LE_GLASS_FRAME_SAMPLES) {
        struct le_glass_break_event event;

        if (le_glass_break_detector_process(
                (struct le_glass_break_detector *)detector,
                signal_buffer + offset, &event)) {
            ++count;
            if (detection_sample)
                *detection_sample = event.detection_sample;
        }
    }
    return count;
}

int main(void)
{
    struct le_glass_break_detector detector;
    uint64_t detection_sample = 0;
    int count;

    le_glass_break_detector_init(&detector);
    fill_room_noise(10.0f);
    add_glass_burst(1U * RATE, 5000.0f, 12.0f);
    add_glass_burst(1U * RATE + 30U * RATE / 100U, 2500.0f, 10.0f);
    count = count_events(&detector, &detection_sample);
    require_condition(count == 0, "silent by default");

    le_glass_break_detector_set_enabled(&detector, 1);
    count = count_events(&detector, &detection_sample);
    require_condition(count == 1, "shatter and tinkle is one break");
    require_condition(detection_sample >= 1U * RATE &&
                      detection_sample < 2U * RATE,
                      "detection sample is at the shatter");

    le_glass_break_detector_set_enabled(&detector, 1);
    fill_room_noise(10.0f);
    add_glass_burst(1U * RATE, 5000.0f, 12.0f);
    count = count_events(&detector, NULL);
    require_condition(count == 0, "one burst alone is not a break");

    le_glass_break_detector_set_enabled(&detector, 1);
    fill_room_noise(10.0f);
    add_voiced(1U * RATE, 4000.0f, 800.0f);
    count = count_events(&detector, NULL);
    require_condition(count == 0, "voiced speech is not a break");

    puts("glass_break_detector: shatter+tinkle, single burst and speech ok");
    return 0;
}
