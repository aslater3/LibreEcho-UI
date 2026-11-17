/*
 * Regression contract for the clap detector.
 *
 * The signals are synthetic: a clap is a few milliseconds of broadband noise
 * with a fast exponential decay; speech-like and music-like sounds are
 * sustained or slowly decaying. These cases pin the decision rules (single,
 * double, decay, crest, flatness, refractory, off by default). The hardware
 * run on real microphones is the evidence for thresholds in a room.
 */

#include "adapter/clap_detector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE LE_CLAP_RATE
#define SECONDS 4
#define TOTAL (RATE * SECONDS)
#define MAX_EVENTS 8

static int16_t signal_buffer[TOTAL];
static uint32_t random_state = 12345U;

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "clap detector regression: %s\n", message);
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

static void add_clap(size_t start, float amplitude)
{
    size_t n;

    /* 4 ms of noise, decaying with a 1.5 ms time constant. */
    for (n = 0; n < 4U * RATE / 1000U && start + n < TOTAL; ++n) {
        float envelope = expf(-(float)n / (1.5f * RATE / 1000.0f));
        float value = signal_buffer[start + n] +
                      random_unit() * amplitude * envelope;

        if (value > 32767.0f) value = 32767.0f;
        if (value < -32768.0f) value = -32768.0f;
        signal_buffer[start + n] = (int16_t)value;
    }
}

/* Speech-like: a harmonic voice with 60 ms attack and a slow 150 ms fall. */
static void add_voiced_syllable(size_t start, float amplitude)
{
    size_t n;

    for (n = 0; n < 250U * RATE / 1000U && start + n < TOTAL; ++n) {
        float t = (float)n / (float)RATE;
        float envelope = n < 60U * RATE / 1000U
            ? (float)n / (60.0f * RATE / 1000.0f)
            : expf(-((float)n - 60.0f * RATE / 1000.0f) / (0.12f * RATE));
        float voice = 0.0f;
        int harmonic;

        for (harmonic = 1; harmonic <= 20; ++harmonic)
            voice += sinf(2.0f * 3.14159265f * 140.0f * (float)harmonic * t) /
                     (float)harmonic;
        signal_buffer[start + n] = (int16_t)(voice * envelope * amplitude);
    }
}

/* Sustained music-like tone with slow vibrato. */
static void add_tone(float amplitude, float hz)
{
    size_t n;

    for (n = 0; n < TOTAL; ++n) {
        float t = (float)n / (float)RATE;
        float value = sinf(2.0f * 3.14159265f * hz * t +
                           0.5f * sinf(2.0f * 3.14159265f * 5.0f * t));

        signal_buffer[n] = (int16_t)(value * amplitude +
                                     signal_buffer[n]);
    }
}

/* Feed the buffer in 10 ms frames and collect every reported event. */
static size_t run_detector(struct le_clap_detector *detector,
                           struct le_clap_event *events)
{
    size_t frame;
    size_t count = 0;

    for (frame = 0; frame + LE_CLAP_FRAME_SAMPLES <= TOTAL;
         frame += LE_CLAP_FRAME_SAMPLES) {
        struct le_clap_event event;

        if (le_clap_detector_process(detector, signal_buffer + frame,
                                     &event)) {
            require_condition(count < MAX_EVENTS, "too many events");
            events[count++] = event;
        }
    }
    return count;
}

static void require_event_near(const struct le_clap_event *event,
                               uint64_t expected_sample,
                               unsigned int claps,
                               const char *message)
{
    int64_t delta = (int64_t)event->detection_sample -
                    (int64_t)expected_sample;

    require_condition(event->claps == claps, message);
    require_condition(delta >= -(int64_t)LE_CLAP_FRAME_SAMPLES * 2 &&
                          delta <= (int64_t)LE_CLAP_FRAME_SAMPLES * 2,
                      "event is not at the clap");
}

static void test_off_by_default_costs_nothing(void)
{
    struct le_clap_detector detector;
    struct le_clap_event events[MAX_EVENTS];

    le_clap_detector_init(&detector);
    fill_room_noise(20.0f);
    add_clap(RATE, 12000.0f);
    require_condition(run_detector(&detector, events) == 0,
                      "a disabled detector reported a clap");
    require_condition(detector.frames == 0,
                      "a disabled detector processed frames");
}

static void test_room_noise_is_silent(void)
{
    struct le_clap_detector detector;
    struct le_clap_event events[MAX_EVENTS];

    le_clap_detector_init(&detector);
    le_clap_detector_set_enabled(&detector, 1);
    fill_room_noise(20.0f);
    require_condition(run_detector(&detector, events) == 0,
                      "room noise produced a clap");
}

static void test_single_clap(void)
{
    struct le_clap_detector detector;
    struct le_clap_event events[MAX_EVENTS];
    size_t count;

    le_clap_detector_init(&detector);
    le_clap_detector_set_enabled(&detector, 1);
    fill_room_noise(20.0f);
    add_clap(RATE, 12000.0f);
    count = run_detector(&detector, events);
    require_condition(count == 1, "one clap was not reported exactly once");
    require_event_near(&events[0], RATE, LE_CLAP_SINGLE,
                       "single clap reported with the wrong claps value");
}

static void test_double_clap(void)
{
    struct le_clap_detector detector;
    struct le_clap_event events[MAX_EVENTS];
    size_t count;

    le_clap_detector_init(&detector);
    le_clap_detector_set_enabled(&detector, 1);
    fill_room_noise(20.0f);
    add_clap(RATE, 12000.0f);
    add_clap(RATE + RATE * 3 / 10, 12000.0f);
    count = run_detector(&detector, events);
    require_condition(count == 1, "two claps 300 ms apart were not one double");
    require_event_near(&events[0], RATE, LE_CLAP_DOUBLE,
                       "double clap reported with the wrong claps value");
}

static void test_claps_far_apart_are_two_singles(void)
{
    struct le_clap_detector detector;
    struct le_clap_event events[MAX_EVENTS];
    size_t count;

    le_clap_detector_init(&detector);
    le_clap_detector_set_enabled(&detector, 1);
    fill_room_noise(20.0f);
    add_clap(RATE, 12000.0f);
    add_clap(RATE * 5 / 2, 12000.0f);
    count = run_detector(&detector, events);
    require_condition(count == 2, "claps 1.5 s apart were not two singles");
    require_condition(events[0].claps == LE_CLAP_SINGLE &&
                          events[1].claps == LE_CLAP_SINGLE,
                      "claps 1.5 s apart were not both singles");
}

static void test_speech_like_is_rejected(void)
{
    struct le_clap_detector detector;
    struct le_clap_event events[MAX_EVENTS];
    size_t n;

    le_clap_detector_init(&detector);
    le_clap_detector_set_enabled(&detector, 1);
    fill_room_noise(20.0f);
    for (n = 0; n < 6; ++n)
        add_voiced_syllable(RATE / 2 + n * RATE / 4, 6000.0f);
    require_condition(run_detector(&detector, events) == 0,
                      "voiced speech-like syllables produced a clap");
}

static void test_sustained_music_is_rejected(void)
{
    struct le_clap_detector detector;
    struct le_clap_event events[MAX_EVENTS];

    le_clap_detector_init(&detector);
    le_clap_detector_set_enabled(&detector, 1);
    fill_room_noise(20.0f);
    add_tone(8000.0f, 440.0f);
    require_condition(run_detector(&detector, events) == 0,
                      "a sustained tone produced a clap");
}

static void test_broadband_sustained_noise_is_rejected(void)
{
    struct le_clap_detector detector;
    struct le_clap_event events[MAX_EVENTS];

    le_clap_detector_init(&detector);
    le_clap_detector_set_enabled(&detector, 1);
    fill_room_noise(6000.0f);
    require_condition(run_detector(&detector, events) == 0,
                      "sustained broadband noise produced a clap");
}

static void test_toggle_clears_state(void)
{
    struct le_clap_detector detector;
    struct le_clap_event events[MAX_EVENTS];
    size_t count;

    le_clap_detector_init(&detector);
    le_clap_detector_set_enabled(&detector, 1);
    fill_room_noise(20.0f);
    add_clap(RATE, 12000.0f);
    /* Run until the clap is pending, then switch off: the pending clap must go. */
    count = 0;
    {
        size_t frame;
        for (frame = 0; frame < (RATE + RATE / 4) / LE_CLAP_FRAME_SAMPLES; ++frame) {
            struct le_clap_event event;
            if (le_clap_detector_process(&detector,
                                         signal_buffer + frame * LE_CLAP_FRAME_SAMPLES,
                                         &event))
                events[count++] = event;
        }
    }
    require_condition(detector.pending_single == 1,
                      "the clap was not pending before the toggle");
    le_clap_detector_set_enabled(&detector, 0);
    require_condition(detector.pending_single == 0,
                      "switching off kept a pending clap");
    require_condition(run_detector(&detector, events) == 0,
                      "a switched-off detector reported a clap");
}

int main(void)
{
    test_off_by_default_costs_nothing();
    test_room_noise_is_silent();
    test_single_clap();
    test_double_clap();
    test_claps_far_apart_are_two_singles();
    test_speech_like_is_rejected();
    test_sustained_music_is_rejected();
    test_broadband_sustained_noise_is_rejected();
    test_toggle_clears_state();
    puts("clap detector: ok");
    return 0;
}
