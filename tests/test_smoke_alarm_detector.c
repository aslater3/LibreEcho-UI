/*
 * Regression contract for the smoke alarm (T3 pattern) detector.
 *
 * The signals are synthetic: a T3-like alarm is three 0.5 s pulses of a
 * steady tone near 3.1 kHz, each followed by a 0.5 s gap. The cases pin the
 * decision rules: a full sequence is reported once, short or wrong-frequency
 * or broadband or speech-like sequences are not, pulse and gap windows are
 * enforced, a continuing alarm is reported once per cooldown, and the
 * detector is off by default. These signals do not measure real alarms; the
 * real-alarm check on a microphone is still owed.
 */

#include "adapter/smoke_alarm_detector.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE LE_SMOKE_RATE
#define FRAME LE_SMOKE_FRAME_SAMPLES
#define SECONDS 30
#define TOTAL (RATE * SECONDS)
#define PI 3.14159265f

static int16_t signal_buffer[TOTAL];
static uint32_t random_state = 4242U;

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "smoke alarm detector regression: %s\n", message);
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

static void add_sample(size_t index, float value)
{
    float mixed;

    if (index >= TOTAL)
        return;
    mixed = signal_buffer[index] + value;
    if (mixed > 32767.0f) mixed = 32767.0f;
    if (mixed < -32768.0f) mixed = -32768.0f;
    signal_buffer[index] = (int16_t)mixed;
}

/* One pulse of a steady tone, starting at start_ms, of length_ms. */
static void add_tone_pulse(size_t start_ms, size_t length_ms, float hz,
                           float amplitude)
{
    size_t n;
    size_t start = start_ms * RATE / 1000U;
    size_t length = length_ms * RATE / 1000U;

    for (n = 0; n < length; ++n)
        add_sample(start + n,
                   amplitude * sinf(2.0f * PI * hz * (float)n / RATE));
}

/* A T3-like train: count pulses of on_ms with off_ms gaps, from start_ms. */
static void add_t3_train(size_t start_ms, unsigned int count,
                         size_t on_ms, size_t off_ms, float hz,
                         float amplitude)
{
    unsigned int i;
    size_t at = start_ms;

    for (i = 0; i < count; ++i) {
        add_tone_pulse(at, on_ms, hz, amplitude);
        at += on_ms + off_ms;
    }
}

/*
 * Feed the signal from from_tenths to to_tenths (in 0.1 s units) through a
 * detector and return the events reported.
 */
static unsigned int run_detector_range(struct le_smoke_alarm_detector *detector,
                                       size_t from_tenths, size_t to_tenths,
                                       struct le_smoke_alarm_event *first)
{
    unsigned int events = 0;
    size_t first_frame = from_tenths * RATE / 10U / FRAME;
    size_t frames = to_tenths * RATE / 10U / FRAME;
    size_t f;

    for (f = first_frame; f < frames; ++f) {
        struct le_smoke_alarm_event event;

        if (le_smoke_alarm_detector_process(detector,
                                            signal_buffer + f * FRAME,
                                            &event)) {
            if (events == 0 && first)
                *first = event;
            ++events;
        }
    }
    return events;
}

static unsigned int run_detector(struct le_smoke_alarm_detector *detector,
                                 size_t seconds_in_tenths,
                                 struct le_smoke_alarm_event *first)
{
    return run_detector_range(detector, 0, seconds_in_tenths, first);
}

static void test_off_by_default(void)
{
    struct le_smoke_alarm_detector detector;
    struct le_smoke_alarm_event event;

    fill_room_noise(20.0f);
    add_t3_train(1000, 3, 500, 500, 3100.0f, 8000.0f);
    le_smoke_alarm_detector_init(&detector);
    require_condition(run_detector(&detector, 100, &event) == 0,
                      "detector reported while off");
    require_condition(detector.frames == 0,
                      "off detector counted frames");
}

static void test_t3_sequence_reports_once(void)
{
    struct le_smoke_alarm_detector detector;
    struct le_smoke_alarm_event event;
    unsigned int events;

    fill_room_noise(20.0f);
    add_t3_train(1000, 3, 500, 500, 3100.0f, 8000.0f);
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    events = run_detector(&detector, 100, &event);
    require_condition(events == 1, "T3 sequence not reported exactly once");
    require_condition(event.pulses == LE_SMOKE_PULSES_REQUIRED,
                      "T3 event pulse count is not 3");
    /* First pulse starts at 1.0 s, so the sample is 16000 (+/- one frame). */
    require_condition(event.detection_sample >= 15840U &&
                      event.detection_sample <= 16160U,
                      "T3 event sample is not the first pulse");
}

static void test_two_pulses_are_not_enough(void)
{
    struct le_smoke_alarm_detector detector;

    fill_room_noise(20.0f);
    add_t3_train(1000, 2, 500, 500, 3100.0f, 8000.0f);
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    require_condition(run_detector(&detector, 100, NULL) == 0,
                      "two pulses reported as an alarm");
}

static void test_pulse_length_window(void)
{
    struct le_smoke_alarm_detector detector;

    /* Pulses of 2 s are a steady tone, not a T3 pulse. */
    fill_room_noise(20.0f);
    add_t3_train(1000, 3, 2000, 500, 3100.0f, 8000.0f);
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    require_condition(run_detector(&detector, 200, NULL) == 0,
                      "2 s pulses reported as T3");
}

static void test_gap_window(void)
{
    struct le_smoke_alarm_detector detector;

    /* Gaps of 2 s break the sequence. */
    fill_room_noise(20.0f);
    add_t3_train(1000, 3, 500, 2000, 3100.0f, 8000.0f);
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    require_condition(run_detector(&detector, 100, NULL) == 0,
                      "2 s gaps reported as T3");
}

static void test_wrong_frequency_ignored(void)
{
    struct le_smoke_alarm_detector detector;

    /* A 1 kHz tone with the same timing is not an alarm band. */
    fill_room_noise(20.0f);
    add_t3_train(1000, 3, 500, 500, 1000.0f, 8000.0f);
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    require_condition(run_detector(&detector, 100, NULL) == 0,
                      "1 kHz pulses reported as T3");
}

static void test_quiet_tone_ignored(void)
{
    struct le_smoke_alarm_detector detector;

    fill_room_noise(20.0f);
    add_t3_train(1000, 3, 500, 500, 3100.0f, 30.0f);
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    require_condition(run_detector(&detector, 100, NULL) == 0,
                      "quiet tone reported as T3");
}

static void test_broadband_bursts_ignored(void)
{
    struct le_smoke_alarm_detector detector;
    size_t pulse;

    /* Three 0.5 s noise bursts with the same timing are not tonal. */
    fill_room_noise(20.0f);
    for (pulse = 0; pulse < 3; ++pulse) {
        size_t n;
        size_t start = (1000U + pulse * 1000U) * RATE / 1000U;

        for (n = 0; n < RATE / 2; ++n)
            add_sample(start + n, random_unit() * 8000.0f);
    }
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    require_condition(run_detector(&detector, 100, NULL) == 0,
                      "broadband bursts reported as T3");
}

static void test_speech_like_ignored(void)
{
    struct le_smoke_alarm_detector detector;
    size_t syllable;

    /* Voiced syllables on a 140 Hz pitch, with 250 ms and 150 ms breaks. */
    fill_room_noise(20.0f);
    for (syllable = 0; syllable < 3; ++syllable) {
        size_t start = (1000U + syllable * 400U) * RATE / 1000U;
        size_t n;

        for (n = 0; n < RATE / 4; ++n) {
            float t = (float)n / (float)RATE;
            float voice = 0.0f;
            int harmonic;

            for (harmonic = 1; harmonic <= 20; ++harmonic)
                voice += sinf(2.0f * PI * 140.0f * (float)harmonic * t) /
                         (float)harmonic;
            add_sample(start + n, voice * 2500.0f);
        }
    }
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    require_condition(run_detector(&detector, 100, NULL) == 0,
                      "speech-like syllables reported as T3");
}

static void test_continuing_alarm_cooldown(void)
{
    struct le_smoke_alarm_detector detector;
    unsigned int events;

    /* Six pulses in 6 s: one report, then the 10 s cooldown holds. */
    fill_room_noise(20.0f);
    add_t3_train(1000, 6, 500, 500, 3100.0f, 8000.0f);
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    events = run_detector(&detector, 100, NULL);
    require_condition(events == 1,
                      "continuing alarm not limited by cooldown");
}

static void test_toggle_clears_sequence(void)
{
    struct le_smoke_alarm_detector detector;

    /* Two pulses, toggle off and on, then the third pulse: no report. */
    fill_room_noise(20.0f);
    add_t3_train(1000, 3, 500, 500, 3100.0f, 8000.0f);
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    /* Control: without the toggle the third pulse completes the sequence. */
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    require_condition(run_detector(&detector, 100, NULL) == 1,
                      "control sequence not reported");
    le_smoke_alarm_detector_init(&detector);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    run_detector_range(&detector, 0, 26, NULL);
    le_smoke_alarm_detector_set_enabled(&detector, 0);
    le_smoke_alarm_detector_set_enabled(&detector, 1);
    require_condition(run_detector_range(&detector, 26, 100, NULL) == 0,
                      "toggle did not clear the pulse sequence");
}

int main(void)
{
    test_off_by_default();
    test_t3_sequence_reports_once();
    test_two_pulses_are_not_enough();
    test_pulse_length_window();
    test_gap_window();
    test_wrong_frequency_ignored();
    test_quiet_tone_ignored();
    test_broadband_bursts_ignored();
    test_speech_like_ignored();
    test_continuing_alarm_cooldown();
    test_toggle_clears_sequence();
    puts("smoke alarm detector: ok");
    return 0;
}
