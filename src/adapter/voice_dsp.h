#ifndef LIBREECHO_VOICE_DSP_H
#define LIBREECHO_VOICE_DSP_H

#include <stddef.h>
#include <stdint.h>

#define LE_VOICE_RATE 16000
#define LE_VOICE_RAW_CHANNELS 9
#define LE_VOICE_LOGICAL_CHANNELS 7
#define LE_VOICE_VALID_BITS 16
#define LE_VOICE_BYTES_PER_SAMPLE 3
#define LE_VOICE_RAW_FRAME_BYTES \
    (LE_VOICE_RAW_CHANNELS * LE_VOICE_BYTES_PER_SAMPLE)
#define LE_VOICE_MICCAL_UNITY 16384
#define LE_VOICE_FRAME_SAMPLES 160
#define LE_VOICE_VAD_DEFAULT_FLOOR_RMS 16U
#define LE_VOICE_WAKE_MIC_MASK \
    ((1U << 0) | (1U << 1) | (1U << 3) | (1U << 4))
#define LE_VOICE_BEAM_CHANNEL_EARLY 0
#define LE_VOICE_BEAM_CHANNEL_LATE 3
#define LE_VOICE_BEAM_DELAY_SAMPLES 4

enum le_voice_calibration_mode {
    LE_VOICE_CALIBRATION_OFF = 0,
    LE_VOICE_CALIBRATION_DIRECT_Q14,
    LE_VOICE_CALIBRATION_INVERSE_Q14
};

struct le_voice_calibration {
    uint16_t q14[LE_VOICE_LOGICAL_CHANNELS];
    uint8_t raw_for_logical[LE_VOICE_LOGICAL_CHANNELS];
    enum le_voice_calibration_mode mode;
};

struct le_voice_highpass {
    int32_t previous_input;
    int32_t previous_output;
};

struct le_voice_beamformer {
    int16_t early_delay[LE_VOICE_BEAM_DELAY_SAMPLES];
    unsigned int write_index;
    unsigned int primed;
};

struct le_voice_vad {
    uint64_t noise_energy;
    uint64_t minimum_energy;
    unsigned int speech_run;
    unsigned int hangover;
    int initialized;
    int active;
};

struct le_voice_vad_result {
    uint64_t frame_energy;
    uint64_t noise_energy;
    int speech;
    int active;
};

void le_voice_calibration_init(
    struct le_voice_calibration *calibration,
    const int q14[LE_VOICE_LOGICAL_CHANNELS],
    const uint8_t raw_for_logical[LE_VOICE_LOGICAL_CHANNELS],
    enum le_voice_calibration_mode mode);

int16_t le_voice_unpack_s24_3le(const uint8_t sample[3]);

void le_voice_decode_calibrated(
    const uint8_t raw_frame[LE_VOICE_RAW_FRAME_BYTES],
    const struct le_voice_calibration *calibration,
    int16_t logical[LE_VOICE_LOGICAL_CHANNELS]);

int16_t le_voice_mix_selected(
    const int16_t logical[LE_VOICE_LOGICAL_CHANNELS],
    unsigned int selected_mask);

void le_voice_beamformer_init(struct le_voice_beamformer *beamformer);
int16_t le_voice_beamformer_sample(
    struct le_voice_beamformer *beamformer,
    const int16_t logical[LE_VOICE_LOGICAL_CHANNELS]);

void le_voice_highpass_init(struct le_voice_highpass *filter);
int16_t le_voice_highpass_sample(struct le_voice_highpass *filter,
                                 int16_t input);

void le_voice_process_interleaved(
    const uint8_t *raw,
    size_t frames,
    const struct le_voice_calibration *calibration,
    unsigned int selected_mask,
    struct le_voice_highpass *highpass,
    int16_t *mono);

void le_voice_process_beamformed_interleaved(
    const uint8_t *raw,
    size_t frames,
    const struct le_voice_calibration *calibration,
    struct le_voice_beamformer *beamformer,
    struct le_voice_highpass *highpass,
    int16_t *mono);

void le_voice_vad_init(struct le_voice_vad *vad);
int le_voice_vad_set_floor_rms(
    struct le_voice_vad *vad, unsigned int floor_rms);
struct le_voice_vad_result le_voice_vad_process(
    struct le_voice_vad *vad,
    const int16_t *samples,
    size_t count);

#endif
