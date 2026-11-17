#ifndef LIBREECHO_VOICE_WHISPER_H
#define LIBREECHO_VOICE_WHISPER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Per-turn whisper classifier over the request audio (16 kHz mono PCM).
 *
 * A whispered request is unvoiced: there is no pitch-periodic glottal source,
 * so the normalised autocorrelation of each active frame stays weak at every
 * pitch lag, and the excitation is noise-like, which puts relatively more
 * energy in the first difference than a voiced vowel does. The classifier
 * works on 20 ms frames, skips quiet frames, and only decides when enough
 * active speech was seen. It is a cheap heuristic for a turn-level flag; it
 * is not a speaker or prosody model.
 */

#define LE_VOICE_WHISPER_FRAME_SAMPLES 320U       /* 20 ms at 16 kHz */
#define LE_VOICE_WHISPER_MIN_ACTIVE_FRAMES 10U    /* 200 ms of speech */
#define LE_VOICE_WHISPER_MAX_ANALYSED_FRAMES 64U  /* bounds the work per turn */

struct le_voice_whisper_result {
    unsigned int active_frames;
    unsigned int unvoiced_frames;
    /*
     * Mean per-frame pitch strength: the squared peak of the normalised
     * autocorrelation over the pitch lags, in permille (0..1000). Voiced
     * speech is near 1000; noise-excited speech is well below 350.
     */
    unsigned int harmonicity_permille;
    /* Mean first-difference energy over signal energy, in permille. */
    unsigned int roughness_permille;
    int whispered;
};

/*
 * Classify count samples of a request. Returns 0 when the result is valid
 * (whispered may still be 0), -1 for bad arguments or when there was not
 * enough active speech to decide; in that case whispered is 0.
 */
int le_voice_whisper_classify(const int16_t *samples, size_t count,
                              struct le_voice_whisper_result *result);

#endif
