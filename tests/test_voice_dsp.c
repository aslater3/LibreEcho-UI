#include "adapter/voice_dsp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

static void pack_s16_as_s24_3le(int16_t value, uint8_t output[3])
{
    uint32_t packed = (uint32_t)(int32_t)value & 0x00ffffffU;

    output[0] = (uint8_t)packed;
    output[1] = (uint8_t)(packed >> 8);
    output[2] = (uint8_t)(packed >> 16);
}

int main(void)
{
    static const uint8_t identity[LE_VOICE_LOGICAL_CHANNELS] =
        {0, 1, 2, 3, 4, 5, 6};
    int gains[LE_VOICE_LOGICAL_CHANNELS] = {
        16384, 8192, 16384, 16384, 32768, 16384, 16384
    };
    struct le_voice_calibration calibration;
    struct le_voice_beamformer beamformer;
    uint8_t raw[LE_VOICE_RAW_FRAME_BYTES];
    uint8_t beam_raw[12 * LE_VOICE_RAW_FRAME_BYTES];
    int16_t logical[LE_VOICE_LOGICAL_CHANNELS];
    int16_t beam_output[12];
    struct le_voice_highpass highpass;
    struct le_voice_vad vad;
    struct le_voice_vad_result vad_result;
    int16_t frame[LE_VOICE_FRAME_SAMPLES];
    int i;

    {
        const uint8_t positive[3] = {0xff, 0x7f, 0x00};
        const uint8_t negative[3] = {0x00, 0x80, 0xff};
        const uint8_t minus_one[3] = {0xff, 0xff, 0xff};

        CHECK(le_voice_unpack_s24_3le(positive) == 32767);
        CHECK(le_voice_unpack_s24_3le(negative) == -32768);
        CHECK(le_voice_unpack_s24_3le(minus_one) == -1);
    }

    memset(raw, 0, sizeof(raw));
    for (i = 0; i < LE_VOICE_LOGICAL_CHANNELS; ++i)
        pack_s16_as_s24_3le(1000, raw + i * 3);
    le_voice_calibration_init(
        &calibration, gains, identity, LE_VOICE_CALIBRATION_DIRECT_Q14);
    le_voice_decode_calibrated(raw, &calibration, logical);
    CHECK(logical[0] == 1000);
    CHECK(logical[1] == 500);
    CHECK(logical[4] == 2000);
    CHECK(le_voice_mix_selected(logical, (1U << 0) | (1U << 1)) == 750);
    CHECK(le_voice_mix_selected(logical, LE_VOICE_WAKE_MIC_MASK) == 1125);

    /*
     * Lane 3 carries the same ramp four samples later than lane 0.  Process
     * across an awkward five-frame boundary to prove the measured delay line
     * remains continuous across capture reads.
     */
    memset(beam_raw, 0, sizeof(beam_raw));
    for (i = 0; i < 12; ++i) {
        int16_t source = (int16_t)((i + 1) * 100);
        int16_t late = i >= LE_VOICE_BEAM_DELAY_SAMPLES
            ? (int16_t)((i + 1 - LE_VOICE_BEAM_DELAY_SAMPLES) * 100)
            : 0;

        pack_s16_as_s24_3le(
            source,
            beam_raw + i * LE_VOICE_RAW_FRAME_BYTES +
                LE_VOICE_BEAM_CHANNEL_EARLY * 3);
        pack_s16_as_s24_3le(
            late,
            beam_raw + i * LE_VOICE_RAW_FRAME_BYTES +
                LE_VOICE_BEAM_CHANNEL_LATE * 3);
    }
    le_voice_calibration_init(
        &calibration, NULL, identity, LE_VOICE_CALIBRATION_OFF);
    le_voice_beamformer_init(&beamformer);
    le_voice_process_beamformed_interleaved(
        beam_raw, 5, &calibration, &beamformer, NULL, beam_output);
    le_voice_process_beamformed_interleaved(
        beam_raw + 5 * LE_VOICE_RAW_FRAME_BYTES, 7,
        &calibration, &beamformer, NULL, beam_output + 5);
    for (i = 0; i < LE_VOICE_BEAM_DELAY_SAMPLES; ++i)
        CHECK(beam_output[i] == 0);
    for (i = LE_VOICE_BEAM_DELAY_SAMPLES; i < 12; ++i)
        CHECK(beam_output[i] ==
              (i + 1 - LE_VOICE_BEAM_DELAY_SAMPLES) * 100);

    le_voice_calibration_init(
        &calibration, gains, identity, LE_VOICE_CALIBRATION_INVERSE_Q14);
    le_voice_decode_calibrated(raw, &calibration, logical);
    CHECK(logical[0] == 1000);
    CHECK(logical[1] == 2000);
    CHECK(logical[4] == 500);

    le_voice_highpass_init(&highpass);
    for (i = 0; i < LE_VOICE_RATE / 2; ++i)
        (void)le_voice_highpass_sample(&highpass, 1000);
    CHECK(le_voice_highpass_sample(&highpass, 1000) >= -1);
    CHECK(le_voice_highpass_sample(&highpass, 1000) <= 1);

    le_voice_vad_init(&vad);
    memset(frame, 0, sizeof(frame));
    for (i = 0; i < 20; ++i) {
        vad_result = le_voice_vad_process(&vad, frame,
                                          LE_VOICE_FRAME_SAMPLES);
        CHECK(!vad_result.active);
    }
    for (i = 0; i < LE_VOICE_FRAME_SAMPLES; ++i)
        frame[i] = (i & 1) ? 3000 : -3000;
    vad_result = le_voice_vad_process(&vad, frame,
                                      LE_VOICE_FRAME_SAMPLES);
    CHECK(vad_result.speech);
    CHECK(!vad_result.active);
    vad_result = le_voice_vad_process(&vad, frame,
                                      LE_VOICE_FRAME_SAMPLES);
    CHECK(vad_result.active);
    memset(frame, 0, sizeof(frame));
    for (i = 0; i < 20; ++i)
        vad_result = le_voice_vad_process(&vad, frame,
                                          LE_VOICE_FRAME_SAMPLES);
    CHECK(vad_result.active);
    vad_result = le_voice_vad_process(&vad, frame,
                                      LE_VOICE_FRAME_SAMPLES);
    CHECK(!vad_result.active);

    le_voice_vad_init(&vad);
    CHECK(le_voice_vad_set_floor_rms(&vad, 0) < 0);
    CHECK(le_voice_vad_set_floor_rms(&vad, 16) == 0);
    CHECK(vad.minimum_energy == 256);
    for (i = 0; i < LE_VOICE_FRAME_SAMPLES; ++i)
        frame[i] = (i & 1) ? 16 : -16;
    vad_result = le_voice_vad_process(&vad, frame,
                                      LE_VOICE_FRAME_SAMPLES);
    CHECK(!vad_result.speech);
    for (i = 0; i < LE_VOICE_FRAME_SAMPLES; ++i)
        frame[i] = (i & 1) ? 80 : -80;
    vad_result = le_voice_vad_process(&vad, frame,
                                      LE_VOICE_FRAME_SAMPLES);
    CHECK(vad_result.speech);
    CHECK(!vad_result.active);
    vad_result = le_voice_vad_process(&vad, frame,
                                      LE_VOICE_FRAME_SAMPLES);
    CHECK(vad_result.active);

    puts("voice_dsp: conversion, calibration, beamforming, HPF and VAD ok");
    return 0;
}
