#include "adapter/voice_aec.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Regression contract for the residual echo suppressor.
 *
 * The signal is synthetic: a repeating linear chirp (the shape of a lot of
 * real playback) through an echo path whose delay changes at five seconds.
 * The sweep keeps the canceller slightly behind, so real residual echo
 * survives, which is the condition the stock extra-suppression stage exists
 * for. The reference is deterministic, so the test does not depend on one
 * lucky noise seed; white noise can leave the canceller in a state where it
 * reports no residual and the suppressor rightly stays out of the way.
 *
 * A second case checks that near-end speech without a reference is not gated.
 * The thresholds are assertions about this construction, not a measured
 * standard.
 */

#define TEST_SECONDS 10
#define TEST_SAMPLES (LE_VOICE_AEC_RATE * TEST_SECONDS)
#define FIRST_DELAY 320
#define SECOND_DELAY 640
#define PATH_CHANGE (LE_VOICE_AEC_RATE * 5U)
#define MEASURE_FROM (LE_VOICE_AEC_RATE * 7U)

static int16_t far_end[TEST_SAMPLES];
static int16_t microphone[TEST_SAMPLES];
static int16_t speech[TEST_SAMPLES];
static uint32_t speech_state = 0x53504543U;

static int16_t next_speech(void)
{
    speech_state = speech_state * 1664525U + 1013904223U;
    return (int16_t)(((speech_state >> 16) & 0x7fffU) - 16384);
}

static double run(int residual, const int16_t *mic)
{
    struct le_voice_aec aec;
    int16_t output[LE_VOICE_AEC_FRAME_SAMPLES];
    double energy = 0.0;
    size_t frame, i;

    if (le_voice_aec_init(&aec, LE_VOICE_AEC_FRAME_SAMPLES,
                          LE_VOICE_AEC_DEFAULT_FILTER_SAMPLES) < 0)
        return -1.0;
    if (le_voice_aec_set_residual_echo(&aec, residual, -40, -10) != 0)
        return -1.0;
    for (frame = 0; frame < TEST_SAMPLES;
         frame += LE_VOICE_AEC_FRAME_SAMPLES) {
        le_voice_aec_process(&aec, mic + frame, far_end + frame, output);
        if (frame >= MEASURE_FROM)
            for (i = 0; i < LE_VOICE_AEC_FRAME_SAMPLES; ++i)
                energy += (double)output[i] * output[i];
    }
    le_voice_aec_destroy(&aec);
    return energy;
}

int main(void)
{
    double off, on, input_energy = 0.0, near_energy;
    size_t i;

    for (i = 0; i < TEST_SAMPLES; ++i) {
        double t = (double)i / LE_VOICE_AEC_RATE;
        double frequency = 100.0 + 3900.0 * fmod(t, 0.5) / 0.5;
        size_t delay = i < PATH_CHANGE ? FIRST_DELAY : SECOND_DELAY;
        int sweep = (int)(8000.0 * sin(2.0 * M_PI * frequency * t));
        int echo;

        far_end[i] = (int16_t)sweep;
        speech[i] = (int16_t)(next_speech() / 2);
        if (i < delay)
            continue;
        echo = far_end[i - delay] / 2;
        echo += (int)(((long)far_end[i - delay] *
                       far_end[i - delay]) >> 17);
        if (echo > 32767) echo = 32767;
        if (echo < -32768) echo = -32768;
        microphone[i] = (int16_t)echo;
    }
    off = run(0, microphone);
    on = run(1, microphone);
    if (off <= 0.0 || on < 0.0 || on > off * 0.85) {
        fprintf(stderr,
                "residual echo suppressor did not cut the reconverging "
                "residual: off=%.0f on=%.0f\n", off, on);
        return 1;
    }

    /*
     * Near-end only: a silent reference must leave speech alone. Measure the
     * input energy and the suppressed output energy across the same window.
     */
    memset(far_end, 0, sizeof(far_end));
    near_energy = run(1, speech);
    for (i = MEASURE_FROM; i < TEST_SAMPLES; ++i)
        input_energy += (double)speech[i] * speech[i];
    if (near_energy <= 0.0 || near_energy < input_energy * 0.5) {
        fprintf(stderr,
                "near-end speech was gated without a reference: "
                "mic=%.0f out=%.0f\n", input_energy, near_energy);
        return 1;
    }

    printf("voice_aec residual: reconverging echo %.0f -> %.0f, "
           "near-end speech kept\n", off, on);
    return 0;
}
