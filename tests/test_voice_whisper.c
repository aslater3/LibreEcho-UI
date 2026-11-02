#define _POSIX_C_SOURCE 200809L

/*
 * Host regression for the per-turn whisper classifier. The signals are
 * synthetic and deterministic: a voiced request is a periodic glottal-like
 * sawtooth at 120 Hz, a whisper is noise-excited (unvoiced) speech-level noise,
 * and the negative cases are silence and too little active speech.
 *
 * Compile with:
 *   cc -D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror \
 *      -Isrc tests/test_voice_whisper.c src/adapter/voice_whisper.c \
 *      -o build/test-voice-whisper
 */

#include "adapter/voice_whisper.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE 16000U
#define SECONDS_SAMPLES (RATE * 2U)

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static uint32_t lcg_state = 12345U;

static uint32_t lcg_next(void)
{
    lcg_state = lcg_state * 1103515245U + 12345U;
    return (lcg_state >> 8) & 0xFFFFU;
}

/* Uniform noise in [-amplitude, amplitude]. */
static int16_t noise_sample(int amplitude)
{
    long value = (long)lcg_next() - 32768L;

    return (int16_t)((value * amplitude) / 32768L);
}

/* Glottal-like periodic excitation with a 120 Hz fundamental. */
static void fill_voiced(int16_t *samples, size_t count, int amplitude)
{
    size_t i;
    const size_t period = RATE / 120U;

    for (i = 0; i < count; ++i) {
        long phase = (long)(i % period);
        long saw = (2L * phase * amplitude) / (long)period - amplitude;

        samples[i] = (int16_t)saw;
    }
}

static void fill_noise(int16_t *samples, size_t count, int amplitude)
{
    size_t i;

    for (i = 0; i < count; ++i)
        samples[i] = noise_sample(amplitude);
}

static int16_t samples_buffer[SECONDS_SAMPLES];

static int test_voiced_request_is_not_whispered(void)
{
    struct le_voice_whisper_result result;

    fill_voiced(samples_buffer, SECONDS_SAMPLES, 6000);
    CHECK(le_voice_whisper_classify(samples_buffer, SECONDS_SAMPLES,
                                    &result) == 0);
    CHECK(result.active_frames >= LE_VOICE_WHISPER_MIN_ACTIVE_FRAMES);
    CHECK(result.whispered == 0);
    CHECK(result.harmonicity_permille >= 700U);
    CHECK(result.unvoiced_frames * 10U < result.active_frames);
    return 0;
}

static int test_noise_excited_request_is_whispered(void)
{
    struct le_voice_whisper_result result;

    fill_noise(samples_buffer, SECONDS_SAMPLES, 2500);
    CHECK(le_voice_whisper_classify(samples_buffer, SECONDS_SAMPLES,
                                    &result) == 0);
    CHECK(result.active_frames >= LE_VOICE_WHISPER_MIN_ACTIVE_FRAMES);
    CHECK(result.unvoiced_frames * 10U >= result.active_frames * 6U);
    CHECK(result.roughness_permille >= 400U);
    CHECK(result.whispered == 1);
    return 0;
}

static int test_loudness_does_not_change_the_class(void)
{
    struct le_voice_whisper_result quiet, loud;

    fill_noise(samples_buffer, SECONDS_SAMPLES, 800);
    CHECK(le_voice_whisper_classify(samples_buffer, SECONDS_SAMPLES,
                                    &quiet) == 0);
    fill_noise(samples_buffer, SECONDS_SAMPLES, 12000);
    CHECK(le_voice_whisper_classify(samples_buffer, SECONDS_SAMPLES,
                                    &loud) == 0);
    CHECK(quiet.whispered == 1);
    CHECK(loud.whispered == 1);
    fill_voiced(samples_buffer, SECONDS_SAMPLES, 1500);
    {
        struct le_voice_whisper_result soft;

        CHECK(le_voice_whisper_classify(samples_buffer, SECONDS_SAMPLES,
                                        &soft) == 0);
        CHECK(soft.whispered == 0);
    }
    return 0;
}

static int test_silence_and_short_audio_are_not_classified(void)
{
    struct le_voice_whisper_result result;

    memset(samples_buffer, 0, sizeof(samples_buffer));
    CHECK(le_voice_whisper_classify(samples_buffer, SECONDS_SAMPLES,
                                    &result) == -1);
    CHECK(result.whispered == 0);

    fill_noise(samples_buffer, SECONDS_SAMPLES, 2500);
    CHECK(le_voice_whisper_classify(
              samples_buffer,
              LE_VOICE_WHISPER_FRAME_SAMPLES *
                  (LE_VOICE_WHISPER_MIN_ACTIVE_FRAMES - 1U),
              &result) == -1);
    CHECK(result.whispered == 0);
    return 0;
}

static int test_bad_arguments_are_rejected(void)
{
    struct le_voice_whisper_result result;

    CHECK(le_voice_whisper_classify(NULL, SECONDS_SAMPLES, &result) == -1);
    CHECK(le_voice_whisper_classify(samples_buffer, SECONDS_SAMPLES,
                                    NULL) == -1);
    return 0;
}

int main(void)
{
    if (test_voiced_request_is_not_whispered() ||
        test_noise_excited_request_is_whispered() ||
        test_loudness_does_not_change_the_class() ||
        test_silence_and_short_audio_are_not_classified() ||
        test_bad_arguments_are_rejected())
        return 1;
    printf("test-voice-whisper: ok\n");
    return 0;
}
