#ifndef LIBREECHO_VOICE_DEREVERB_H
#define LIBREECHO_VOICE_DEREVERB_H

#include <stddef.h>
#include <stdint.h>

/*
 * Late-reverberation suppressor for the microphone path, modelled on the
 * stock ASP "CDR Dereverberator" stage.
 *
 * A far-field talker reaches the array through a direct path plus a decaying
 * tail of room reflections. The tail smears each syllable into the next and
 * costs both the wake word and recognition. This stage estimates the late
 * tail from the recent observed power with an exponential decay and subtracts
 * it per frequency bin, leaving the direct and early sound alone.
 *
 * The estimate uses the power from LE_VOICE_DEREVERB_DELAY_FRAMES blocks ago,
 * so the direct sound is never subtracted from itself, and accumulates with
 * LE_VOICE_DEREVERB_DECAY (roughly a 0.3 s T60 at this hop). Every buffer is
 * fixed-size inside the struct; nothing allocates.
 *
 * le_voice_dereverb_process() rewrites samples in place with a constant delay
 * of LE_VOICE_DEREVERB_LATENCY_SAMPLES (16 ms). Callers that index audio by
 * sample keep their indices: the cleaned stream is the same stream, 16 ms
 * later.
 *
 * The decay and delay are tuned constants, not a measured room model. Unit
 * tests pin them on a synthetic exponential tail; a real room is still owed.
 */
#define LE_VOICE_DEREVERB_RATE 16000
#define LE_VOICE_DEREVERB_FFT 256U
#define LE_VOICE_DEREVERB_HOP 128U
#define LE_VOICE_DEREVERB_BINS (LE_VOICE_DEREVERB_FFT / 2U + 1U)
#define LE_VOICE_DEREVERB_LATENCY_SAMPLES LE_VOICE_DEREVERB_FFT
/* Deepest cut applied to a bin judged to be late reverberation only. */
#define LE_VOICE_DEREVERB_DEFAULT_ATTENUATION_DB 12
#define LE_VOICE_DEREVERB_MIN_ATTENUATION_DB 3
#define LE_VOICE_DEREVERB_MAX_ATTENUATION_DB 24
/* First tail frame after the direct sound, and the per-frame decay. */
#define LE_VOICE_DEREVERB_DELAY_FRAMES 6U
#define LE_VOICE_DEREVERB_HISTORY_FRAMES 16U
#define LE_VOICE_DEREVERB_DECAY 0.83f

struct le_voice_dereverb {
    float window[LE_VOICE_DEREVERB_FFT];
    float twiddle_re[LE_VOICE_DEREVERB_FFT / 2U];
    float twiddle_im[LE_VOICE_DEREVERB_FFT / 2U];
    unsigned short bit_reverse[LE_VOICE_DEREVERB_FFT];
    float frame[LE_VOICE_DEREVERB_FFT];
    float overlap[LE_VOICE_DEREVERB_FFT];
    /* Ring of observed power spectra, one per past block. */
    float history[LE_VOICE_DEREVERB_HISTORY_FRAMES][LE_VOICE_DEREVERB_BINS];
    float late[LE_VOICE_DEREVERB_BINS];
    float floor_gain;
    int16_t input[LE_VOICE_DEREVERB_HOP];
    int16_t output[LE_VOICE_DEREVERB_HOP];
    unsigned int position;
    unsigned int history_position;
    unsigned int history_count;
    unsigned int frames;
};

/* attenuation_db is the deepest cut applied to a reverberation-only bin. */
int le_voice_dereverb_init(struct le_voice_dereverb *dereverb,
                           int attenuation_db);
void le_voice_dereverb_reset(struct le_voice_dereverb *dereverb);
void le_voice_dereverb_process(struct le_voice_dereverb *dereverb,
                               int16_t *samples, size_t count);
/* Change the floor cut without losing the learned tail. Values are clamped. */
void le_voice_dereverb_set_attenuation(struct le_voice_dereverb *dereverb,
                                       int attenuation_db);

#endif /* LIBREECHO_VOICE_DEREVERB_H */
