/* Capture AGC and compressor (#capture-agc). */
#include "adapter/voice_agc.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RATE 16000
#define SAMPLES (RATE * 2)
#define TONE_HZ 440

static int failures;

#define CHECK(condition, ...) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); ++failures; } } while (0)

static double peak(const int16_t *samples, size_t count)
{
    double value = 0.0;
    size_t i;

    for (i = 0; i < count; ++i) {
        double magnitude = fabs((double)samples[i]);

        if (magnitude > value)
            value = magnitude;
    }
    return value;
}

static void fill_tone(int16_t *samples, double amplitude)
{
    size_t i;

    for (i = 0; i < SAMPLES; ++i)
        samples[i] = (int16_t)lrint(amplitude * 32767.0 *
                                    sin(2.0 * 3.14159265358979323846 *
                                        (double)TONE_HZ * (double)i /
                                        (double)RATE));
}

static void test_quiet_speech_is_lifted(void)
{
    struct le_voice_agc agc;
    int16_t samples[SAMPLES];
    double before, after;

    fill_tone(samples, 0.02);
    before = peak(samples, SAMPLES);
    CHECK(le_voice_agc_init(&agc, RATE, 1) == 0, "init failed");
    le_voice_agc_process(&agc, samples, SAMPLES);
    after = peak(samples + SAMPLES / 2, SAMPLES / 2);
    CHECK(after > before * 1.5,
          "quiet tone not lifted: before %.0f after %.0f", before, after);
    CHECK(le_voice_agc_gain(&agc) > 1.0f, "gain did not rise");
}

static void test_loud_speech_is_compressed(void)
{
    struct le_voice_agc agc;
    int16_t samples[SAMPLES];
    double after;

    fill_tone(samples, 0.95);
    CHECK(le_voice_agc_init(&agc, RATE, 1) == 0, "init failed");
    le_voice_agc_process(&agc, samples, SAMPLES);
    after = peak(samples + SAMPLES / 2, SAMPLES / 2);
    CHECK(after < 0.95 * 32767.0,
          "loud tone not compressed: peak %.0f", after);
    CHECK(after <= 32767.0, "output clipped to int16");
}

static void test_noise_floor_is_not_pumped(void)
{
    struct le_voice_agc agc;
    int16_t samples[SAMPLES];
    double before, after;

    fill_tone(samples, 0.001);
    before = peak(samples, SAMPLES);
    CHECK(le_voice_agc_init(&agc, RATE, 1) == 0, "init failed");
    le_voice_agc_process(&agc, samples, SAMPLES);
    after = peak(samples, SAMPLES);
    CHECK(fabs(after - before) < 30.0,
          "floor was lifted: before %.0f after %.0f", before, after);
}

static void test_disabled_is_identity(void)
{
    struct le_voice_agc agc;
    int16_t samples[SAMPLES];
    int16_t copy[SAMPLES];

    fill_tone(samples, 0.2);
    memcpy(copy, samples, sizeof(samples));
    CHECK(le_voice_agc_init(&agc, RATE, 0) == 0, "init failed");
    le_voice_agc_process(&agc, samples, SAMPLES);
    CHECK(memcmp(samples, copy, sizeof(samples)) == 0,
          "disabled AGC changed samples");
    CHECK(le_voice_agc_gain(&agc) == 1.0f, "disabled gain was not unity");
}

static void test_bad_arguments(void)
{
    struct le_voice_agc agc;

    CHECK(le_voice_agc_init(NULL, RATE, 1) < 0, "NULL context accepted");
    CHECK(le_voice_agc_init(&agc, 0, 1) < 0, "zero rate accepted");
}

int main(void)
{
    test_quiet_speech_is_lifted();
    test_loud_speech_is_compressed();
    test_noise_floor_is_not_pumped();
    test_disabled_is_identity();
    test_bad_arguments();
    if (failures) {
        fprintf(stderr, "%d AGC test(s) failed\n", failures);
        return 1;
    }
    printf("test-voice-agc: ok\n");
    return 0;
}
