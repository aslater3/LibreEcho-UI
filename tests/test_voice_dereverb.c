#include "adapter/voice_dereverb.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Regression contract for the late-reverberation suppressor.
 *
 * A dry signal of short harmonic bursts is passed through an exponential
 * synthetic room tail. The measured tail window sits after the direct sound
 * has ended, so it holds reverberation only. Enabling the suppressor must cut
 * that window while leaving the bursts themselves mostly intact. The room is
 * synthetic and the decay is a tuned constant; a real room is still owed.
 */

#define SECONDS 8
#define SAMPLES (LE_VOICE_DEREVERB_RATE * SECONDS)
#define BURSTS 8
#define BURST_START_MS 500
#define BURST_MS 120
#define PERIOD_MS 900
#define TAIL_START_MS 200
#define TAIL_END_MS 400
#define TAIL_FIRST_SAMPLE 320
#define TAIL_SAMPLES 4000
#define TAIL_GAIN 0.5
#define TAIL_TAU 800.0

static int16_t dry[SAMPLES];
static int16_t reverb[SAMPLES];

static double run(int enabled, double *burst_energy)
{
    struct le_voice_dereverb dereverb;
    int16_t output[SAMPLES];
    double tail = 0.0, burst = 0.0;
    size_t i;

    memcpy(output, reverb, sizeof(output));
    if (enabled &&
        le_voice_dereverb_init(&dereverb, 12) < 0)
        return -1.0;
    if (enabled)
        le_voice_dereverb_process(&dereverb, output, SAMPLES);
    for (i = 0; i < BURSTS; ++i) {
        size_t start = (size_t)(BURST_START_MS + i * PERIOD_MS) *
                       LE_VOICE_DEREVERB_RATE / 1000U;
        size_t from = start + (size_t)TAIL_START_MS *
                              LE_VOICE_DEREVERB_RATE / 1000U;
        size_t to = start + (size_t)TAIL_END_MS *
                            LE_VOICE_DEREVERB_RATE / 1000U;
        size_t j;

        for (j = start; j < start + (size_t)BURST_MS *
                                    LE_VOICE_DEREVERB_RATE / 1000U; ++j)
            burst += (double)output[j] * output[j];
        for (j = from; j < to; ++j)
            tail += (double)output[j] * output[j];
    }
    *burst_energy = burst;
    return tail;
}

int main(void)
{
    double tail_off, tail_on, burst_off, burst_on;
    size_t i, b;

    memset(dry, 0, sizeof(dry));
    memset(reverb, 0, sizeof(reverb));
    for (b = 0; b < BURSTS; ++b) {
        size_t start = (size_t)(BURST_START_MS + b * PERIOD_MS) *
                       LE_VOICE_DEREVERB_RATE / 1000U;
        size_t length = (size_t)BURST_MS * LE_VOICE_DEREVERB_RATE / 1000U;

        for (i = 0; i < length; ++i) {
            double t = (double)i / LE_VOICE_DEREVERB_RATE;
            double envelope = sin(3.14159265358979323846 *
                                  (double)i / (double)length);

            dry[start + i] = (int16_t)(envelope * 8000.0 *
                (sin(2.0 * 3.14159265358979323846 * 300.0 * t) +
                 sin(2.0 * 3.14159265358979323846 * 900.0 * t) +
                 sin(2.0 * 3.14159265358979323846 * 1500.0 * t)) / 3.0);
        }
    }
    /* y[n] = x[n] + an exponential tail starting after the direct sound. */
    {
        double tail_sum = (1.0 - exp(-(double)TAIL_SAMPLES / TAIL_TAU)) /
                          (1.0 - exp(-1.0 / TAIL_TAU));

        for (i = 0; i < SAMPLES; ++i) {
            int value = (int)(0.7 * (double)dry[i]);

            for (b = 0; b < TAIL_SAMPLES; ++b) {
                size_t delay = TAIL_FIRST_SAMPLE + b;

                if (i < delay)
                    break;
                value += (int)(TAIL_GAIN * exp(-(double)b / TAIL_TAU) /
                               sqrt(tail_sum) * (double)dry[i - delay]);
            }
            if (value > 32767) value = 32767;
            if (value < -32768) value = -32768;
            reverb[i] = (int16_t)value;
        }
    }

    tail_off = run(0, &burst_off);
    tail_on = run(1, &burst_on);
    printf("tail off=%.0f on=%.0f ratio=%.2f\n",
           tail_off, tail_on, tail_off > 0 ? tail_on / tail_off : 0.0);
    printf("burst off=%.0f on=%.0f ratio=%.2f\n",
           burst_off, burst_on,
           burst_off > 0 ? burst_on / burst_off : 0.0);
    if (tail_off <= 0.0 || tail_on < 0.0 || tail_on > tail_off * 0.6) {
        fprintf(stderr, "late tail was not suppressed\n");
        return 1;
    }
    if (burst_off <= 0.0 || burst_on < burst_off * 0.7) {
        fprintf(stderr, "direct bursts were cut too hard\n");
        return 1;
    }
    return 0;
}
