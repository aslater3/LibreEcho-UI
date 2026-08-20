#include "voice_dsp.h"

#include <limits.h>
#include <string.h>

/*
 * The MT8163 transport advertises packed S24_3LE with 16 useful bits in the
 * low part of each word.  Preserve that observed alignment when converting to
 * the S16 consumer contract; shifting right by eight would discard the live
 * microphone signal.
 */
int16_t le_voice_unpack_s24_3le(const uint8_t sample[3])
{
    int32_t value = (int32_t)sample[0] |
                    ((int32_t)sample[1] << 8) |
                    ((int32_t)sample[2] << 16);

    if (value & 0x00800000)
        value -= 0x01000000;
    if (value > INT16_MAX)
        return INT16_MAX;
    if (value < INT16_MIN)
        return INT16_MIN;
    return (int16_t)value;
}

static int16_t saturate_s16(int64_t value)
{
    if (value > INT16_MAX)
        return INT16_MAX;
    if (value < INT16_MIN)
        return INT16_MIN;
    return (int16_t)value;
}

static int64_t divide_rounded(int64_t numerator, int64_t denominator)
{
    if (denominator <= 0)
        return 0;
    if (numerator >= 0)
        return (numerator + denominator / 2) / denominator;
    return -((-numerator + denominator / 2) / denominator);
}

void le_voice_calibration_init(
    struct le_voice_calibration *calibration,
    const int q14[LE_VOICE_LOGICAL_CHANNELS],
    const uint8_t raw_for_logical[LE_VOICE_LOGICAL_CHANNELS],
    enum le_voice_calibration_mode mode)
{
    size_t logical;

    memset(calibration, 0, sizeof(*calibration));
    calibration->mode = mode;
    for (logical = 0; logical < LE_VOICE_LOGICAL_CHANNELS; ++logical) {
        int gain = q14 ? q14[logical] : LE_VOICE_MICCAL_UNITY;
        uint8_t raw = raw_for_logical
            ? raw_for_logical[logical] : (uint8_t)logical;

        if (gain <= 0 || gain > UINT16_MAX)
            gain = LE_VOICE_MICCAL_UNITY;
        if (raw >= LE_VOICE_LOGICAL_CHANNELS)
            raw = (uint8_t)logical;
        calibration->q14[logical] = (uint16_t)gain;
        calibration->raw_for_logical[logical] = raw;
    }
}

static int16_t apply_calibration(
    int16_t sample,
    uint16_t gain,
    enum le_voice_calibration_mode mode)
{
    int64_t scaled;

    if (mode == LE_VOICE_CALIBRATION_OFF)
        return sample;
    if (mode == LE_VOICE_CALIBRATION_INVERSE_Q14) {
        if (gain == 0)
            return sample;
        scaled = divide_rounded(
            (int64_t)sample * LE_VOICE_MICCAL_UNITY, gain);
    } else {
        scaled = divide_rounded(
            (int64_t)sample * gain, LE_VOICE_MICCAL_UNITY);
    }
    return saturate_s16(scaled);
}

void le_voice_decode_calibrated(
    const uint8_t raw_frame[LE_VOICE_RAW_FRAME_BYTES],
    const struct le_voice_calibration *calibration,
    int16_t logical[LE_VOICE_LOGICAL_CHANNELS])
{
    size_t channel;

    for (channel = 0; channel < LE_VOICE_LOGICAL_CHANNELS; ++channel) {
        unsigned int raw_channel = calibration->raw_for_logical[channel];
        const uint8_t *sample =
            raw_frame + raw_channel * LE_VOICE_BYTES_PER_SAMPLE;

        logical[channel] = apply_calibration(
            le_voice_unpack_s24_3le(sample),
            calibration->q14[channel],
            calibration->mode);
    }
}

int16_t le_voice_mix_selected(
    const int16_t logical[LE_VOICE_LOGICAL_CHANNELS],
    unsigned int selected_mask)
{
    int64_t sum = 0;
    unsigned int count = 0;
    size_t channel;

    for (channel = 0; channel < LE_VOICE_LOGICAL_CHANNELS; ++channel) {
        if (selected_mask & (1U << channel)) {
            sum += logical[channel];
            ++count;
        }
    }
    if (count == 0)
        return 0;
    return saturate_s16(divide_rounded(sum, count));
}

void le_voice_beamformer_init(struct le_voice_beamformer *beamformer)
{
    memset(beamformer, 0, sizeof(*beamformer));
}

int16_t le_voice_beamformer_sample(
    struct le_voice_beamformer *beamformer,
    const int16_t logical[LE_VOICE_LOGICAL_CHANNELS])
{
    int16_t delayed_early =
        beamformer->early_delay[beamformer->write_index];

    /*
     * Directional calibration takes at 0, 90 and 270 degrees consistently
     * measured logical lane 3 four 16 kHz samples behind lane 0.  A causal
     * delay-and-sum therefore delays lane 0 by four samples and combines it
     * with lane 3.  Keeping the delay state here makes the result independent
     * of tinycap read boundaries.
     */
    beamformer->early_delay[beamformer->write_index] =
        logical[LE_VOICE_BEAM_CHANNEL_EARLY];
    beamformer->write_index =
        (beamformer->write_index + 1U) % LE_VOICE_BEAM_DELAY_SAMPLES;
    if (beamformer->primed < LE_VOICE_BEAM_DELAY_SAMPLES) {
        ++beamformer->primed;
        return 0;
    }
    return saturate_s16(divide_rounded(
        (int64_t)delayed_early +
        logical[LE_VOICE_BEAM_CHANNEL_LATE], 2));
}

void le_voice_highpass_init(struct le_voice_highpass *filter)
{
    memset(filter, 0, sizeof(*filter));
}

int16_t le_voice_highpass_sample(struct le_voice_highpass *filter,
                                 int16_t input)
{
    /*
     * First-order 80 Hz high-pass at 16 kHz:
     * y[n] = 0.9691 * (y[n-1] + x[n] - x[n-1]).
     * The Q15 coefficient keeps the hot path integer-only on ARMv7.
     */
    const int32_t coefficient_q15 = 31755;
    int64_t difference = (int64_t)filter->previous_output +
                         input - filter->previous_input;
    /*
     * Truncate toward zero here rather than rounding.  Rounding leaves a
     * small fixed-point limit cycle after a DC step; truncation lets the
     * state decay completely to zero.
     */
    int32_t output = (int32_t)(
        (difference * coefficient_q15) / 32768);

    filter->previous_input = input;
    filter->previous_output = output;
    return saturate_s16(output);
}

void le_voice_process_interleaved(
    const uint8_t *raw,
    size_t frames,
    const struct le_voice_calibration *calibration,
    unsigned int selected_mask,
    struct le_voice_highpass *highpass,
    int16_t *mono)
{
    size_t frame;

    for (frame = 0; frame < frames; ++frame) {
        int16_t logical[LE_VOICE_LOGICAL_CHANNELS];
        int16_t mixed;

        le_voice_decode_calibrated(
            raw + frame * LE_VOICE_RAW_FRAME_BYTES,
            calibration, logical);
        mixed = le_voice_mix_selected(logical, selected_mask);
        mono[frame] = highpass
            ? le_voice_highpass_sample(highpass, mixed) : mixed;
    }
}

void le_voice_process_beamformed_interleaved(
    const uint8_t *raw,
    size_t frames,
    const struct le_voice_calibration *calibration,
    struct le_voice_beamformer *beamformer,
    struct le_voice_highpass *highpass,
    int16_t *mono)
{
    size_t frame;

    for (frame = 0; frame < frames; ++frame) {
        int16_t logical[LE_VOICE_LOGICAL_CHANNELS];
        int16_t mixed;

        le_voice_decode_calibrated(
            raw + frame * LE_VOICE_RAW_FRAME_BYTES,
            calibration, logical);
        mixed = le_voice_beamformer_sample(beamformer, logical);
        mono[frame] = highpass
            ? le_voice_highpass_sample(highpass, mixed) : mixed;
    }
}

void le_voice_vad_init(struct le_voice_vad *vad)
{
    memset(vad, 0, sizeof(*vad));
    vad->minimum_energy =
        LE_VOICE_VAD_DEFAULT_FLOOR_RMS *
        LE_VOICE_VAD_DEFAULT_FLOOR_RMS;
}

int le_voice_vad_set_floor_rms(
    struct le_voice_vad *vad, unsigned int floor_rms)
{
    uint64_t minimum_energy;

    if (!vad || floor_rms < 1U || floor_rms > 1024U)
        return -1;
    minimum_energy = (uint64_t)floor_rms * floor_rms;
    vad->minimum_energy = minimum_energy;
    if (vad->noise_energy < minimum_energy)
        vad->noise_energy = minimum_energy;
    return 0;
}

struct le_voice_vad_result le_voice_vad_process(
    struct le_voice_vad *vad,
    const int16_t *samples,
    size_t count)
{
    /*
     * Array calibration measured an idle RMS around 12-17 and one-metre
     * speech around 70-105 after beamforming.  The old 96 RMS floor made
     * the effective six-times-energy gate about 235 RMS, so a high wake
     * score from normal or distant speech could never be accepted.
     */
    const unsigned int onset_frames = 2;
    const unsigned int hangover_frames = 20;
    struct le_voice_vad_result result;
    uint64_t sum = 0;
    uint64_t threshold;
    size_t i;

    memset(&result, 0, sizeof(result));
    if (count == 0)
        return result;
    for (i = 0; i < count; ++i) {
        int64_t sample = samples[i];
        sum += (uint64_t)(sample * sample);
    }
    result.frame_energy = sum / count;

    if (!vad->initialized) {
        vad->noise_energy = result.frame_energy > vad->minimum_energy
            ? result.frame_energy : vad->minimum_energy;
        vad->initialized = 1;
    }
    threshold = vad->noise_energy * 6U;
    if (threshold < vad->minimum_energy * 6U)
        threshold = vad->minimum_energy * 6U;
    result.speech = result.frame_energy > threshold;

    if (result.speech) {
        if (vad->speech_run < onset_frames)
            ++vad->speech_run;
        if (vad->speech_run >= onset_frames) {
            vad->active = 1;
            vad->hangover = hangover_frames;
        }
    } else {
        vad->speech_run = 0;
        if (vad->active) {
            if (vad->hangover > 0)
                --vad->hangover;
            else
                vad->active = 0;
        }
        if (!vad->active) {
            /* Slow attack prevents a short speech burst becoming "noise". */
            vad->noise_energy =
                (vad->noise_energy * 31U + result.frame_energy) / 32U;
            if (vad->noise_energy < vad->minimum_energy)
                vad->noise_energy = vad->minimum_energy;
        }
    }
    result.noise_energy = vad->noise_energy;
    result.active = vad->active;
    return result;
}
