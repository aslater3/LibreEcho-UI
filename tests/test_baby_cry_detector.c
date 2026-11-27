/*
 * Regression contract for the baby-cry detector.
 *
 * The signals are synthetic: a cry is two high-pitched harmonic bouts
 * separated by a pause; adult male speech is a low-pitched harmonic voice;
 * room noise is broadband. These cases pin the decision rules (voiced run of
 * 450 ms, two bouts inside four seconds, 250-700 Hz, off by default). The
 * hardware run on real microphones is the evidence for the thresholds.
 */

#include "adapter/baby_cry_detector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE LE_CRY_RATE
#define SECONDS 5
#define TOTAL (RATE * SECONDS)

static int16_t signal_buffer[TOTAL];
static uint32_t random_state = 999U;

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "baby cry detector regression: %s\n", message);
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
 * A cry bout: harmonics of ~420 Hz with a little vibrato and a raised-cosine
 * envelope.  Infant cries are strongly voiced and harmonically rich.
 */
static void add_cry_bout(size_t start, float amplitude, float ms)
{
    size_t length = (size_t)(ms * (float)RATE / 1000.0f);
    size_t n;

    for (n = 0; n < length && start + n < TOTAL; ++n) {
        float t = (float)n / (float)RATE;
        float f0 = 420.0f * (1.0f + 0.03f * sinf(2.0f * 3.14159265f * 6.0f * t));
        float envelope = sinf(3.14159265f * (float)n / (float)length);
        float voice = 0.0f;
        int harmonic;

        for (harmonic = 1; harmonic <= 10; ++harmonic)
            voice += sinf(2.0f * 3.14159265f * f0 * (float)harmonic * t) /
                     (float)harmonic;
        signal_buffer[start + n] +=
            (int16_t)(voice * envelope * amplitude);
    }
}

/* Adult male speech: 120 Hz, below the infant range. */
static void add_voiced(size_t start, float amplitude, float ms)
{
    size_t length = (size_t)(ms * (float)RATE / 1000.0f);
    size_t n;

    for (n = 0; n < length && start + n < TOTAL; ++n) {
        float t = (float)n / (float)RATE;
        float envelope = sinf(3.14159265f * (float)n / (float)length);
        float voice = 0.0f;
        int harmonic;

        for (harmonic = 1; harmonic <= 10; ++harmonic)
            voice += sinf(2.0f * 3.14159265f * 120.0f * (float)harmonic * t) /
                     (float)harmonic;
        signal_buffer[start + n] +=
            (int16_t)(voice * envelope * amplitude);
    }
}

static int count_events(const struct le_baby_cry_detector *detector,
                        uint64_t *detection_sample)
{
    size_t offset;
    int count = 0;

    for (offset = 0; offset + LE_CRY_FRAME_SAMPLES <= TOTAL;
         offset += LE_CRY_FRAME_SAMPLES) {
        struct le_baby_cry_event event;

        if (le_baby_cry_detector_process(
                (struct le_baby_cry_detector *)detector,
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
    struct le_baby_cry_detector detector;
    uint64_t detection_sample = 0;
    int count;

    le_baby_cry_detector_init(&detector);
    fill_room_noise(10.0f);
    add_cry_bout(1U * RATE, 3000.0f, 700.0f);
    add_cry_bout(2U * RATE + 10U * RATE / 100U, 3000.0f, 700.0f);
    count = count_events(&detector, &detection_sample);
    require_condition(count == 0, "silent by default");

    le_baby_cry_detector_set_enabled(&detector, 1);
    count = count_events(&detector, &detection_sample);
    require_condition(count == 1, "two cry bouts are one cry");
    require_condition(detection_sample >= 1U * RATE &&
                      detection_sample < 3U * RATE,
                      "detection sample is at the first bout");

    le_baby_cry_detector_set_enabled(&detector, 1);
    fill_room_noise(10.0f);
    add_cry_bout(1U * RATE, 3000.0f, 700.0f);
    count = count_events(&detector, NULL);
    require_condition(count == 0, "one bout alone is not a cry");

    le_baby_cry_detector_set_enabled(&detector, 1);
    fill_room_noise(10.0f);
    add_voiced(1U * RATE, 4000.0f, 700.0f);
    add_voiced(2U * RATE + 10U * RATE / 100U, 4000.0f, 700.0f);
    count = count_events(&detector, NULL);
    require_condition(count == 0, "adult male speech is not a cry");

    le_baby_cry_detector_set_enabled(&detector, 1);
    fill_room_noise(30.0f);
    count = count_events(&detector, NULL);
    require_condition(count == 0, "broadband room noise is not a cry");

    puts("baby_cry_detector: bouts, single bout, speech and noise ok");
    return 0;
}
