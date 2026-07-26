#include "adapter/voice_aec.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_SECONDS 10
#define TEST_SAMPLES (LE_VOICE_AEC_RATE * TEST_SECONDS)
#define ECHO_DELAY_SAMPLES 320

static int16_t far_end[TEST_SAMPLES];
static int16_t microphone[TEST_SAMPLES];

static uint32_t random_state = 0x4c454348U;

static int16_t next_noise(void)
{
    random_state = random_state * 1664525U + 1013904223U;
    return (int16_t)(((random_state >> 16) & 0x7fffU) - 16384);
}

int main(void)
{
    struct le_voice_aec aec;
    int16_t output[LE_VOICE_AEC_FRAME_SAMPLES];
    uint64_t microphone_energy = 0;
    uint64_t output_energy = 0;
    size_t measured = 0;
    size_t frame;
    size_t i;

    for (i = 0; i < TEST_SAMPLES; ++i) {
        far_end[i] = next_noise();
        microphone[i] = i >= ECHO_DELAY_SAMPLES
            ? (int16_t)(far_end[i - ECHO_DELAY_SAMPLES] / 2) : 0;
    }
    if (le_voice_aec_init(&aec, LE_VOICE_AEC_FRAME_SAMPLES,
                          LE_VOICE_AEC_DEFAULT_FILTER_SAMPLES) < 0) {
        fprintf(stderr, "unable to initialize AEC\n");
        return 1;
    }
    for (frame = 0; frame < TEST_SAMPLES;
         frame += LE_VOICE_AEC_FRAME_SAMPLES) {
        le_voice_aec_process(&aec, microphone + frame, far_end + frame,
                             output);
        if (frame >= LE_VOICE_AEC_RATE * 7U) {
            for (i = 0; i < LE_VOICE_AEC_FRAME_SAMPLES; ++i) {
                int64_t mic = microphone[frame + i];
                int64_t out = output[i];

                microphone_energy += (uint64_t)(mic * mic);
                output_energy += (uint64_t)(out * out);
                ++measured;
            }
        }
    }
    le_voice_aec_destroy(&aec);
    if (measured == 0 || output_energy == 0 ||
        output_energy * 10U >= microphone_energy) {
        fprintf(stderr,
                "AEC did not achieve 10 dB suppression: mic=%llu out=%llu\n",
                (unsigned long long)microphone_energy,
                (unsigned long long)output_energy);
        return 1;
    }
    printf("voice_aec: fixed-point 20 ms delayed echo suppressed "
           "(energy ratio %.1f:1)\n",
           (double)microphone_energy / (double)output_energy);
    return 0;
}
