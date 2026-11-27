#ifndef LE_VOICE_AGC_H
#define LE_VOICE_AGC_H

#include <stddef.h>
#include <stdint.h>

/*
 * Capture AGC and soft-knee compressor.
 *
 * The stock ASP front end has an AGC ("Full AGCConfig struct content:",
 * "m_enableAGC", "m_compressorAttack", "m_compressorDecay",
 * "m_compressorRatio", "m_compressorThresholdDB") and a MultiBandCompressor.
 * This implements the full-band part only: an envelope follower drives a
 * smoothed gain that lifts quiet speech toward the threshold and attenuates
 * loud speech above it. There is no lookahead and no multiband split.
 *
 * Levels are linear sample amplitudes. The defaults are judgement calls; the
 * stock numbers live in AFE.cfg, which was not in the dump.
 */
struct le_voice_agc {
    unsigned int rate;
    float threshold;
    float ratio;
    float max_gain;
    float min_gain;
    float gate;
    float attack_coeff;
    float release_coeff;
    float gain_attack_coeff;
    float gain_release_coeff;
    float envelope;
    float gain;
    int primed;
};

/* enabled 0 leaves samples untouched. Returns 0, or -1 on bad parameters. */
int le_voice_agc_init(struct le_voice_agc *agc, unsigned int rate, int enabled);
void le_voice_agc_process(struct le_voice_agc *agc, int16_t *samples,
                          size_t count);
float le_voice_agc_gain(const struct le_voice_agc *agc);

#endif
