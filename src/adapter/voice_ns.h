#ifndef LIBREECHO_VOICE_NS_H
#define LIBREECHO_VOICE_NS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Steady-noise suppressor for the speech sent to the assistant (#401).
 *
 * A short-time spectral Wiener filter: 256-point frames at 16 kHz with a
 * 128-sample hop, square-root Hann analysis and synthesis windows, a
 * minimum-statistics noise estimate per bin (the minimum of the smoothed
 * power over the last ~1.5 s, so a louder room is learnt within about two
 * seconds) and a decision-directed a-priori SNR. Every buffer is fixed-size
 * inside the struct; nothing allocates.
 *
 * le_voice_ns_process() rewrites samples in place with a constant delay of
 * LE_VOICE_NS_LATENCY_SAMPLES (16 ms). Callers that index audio by sample
 * keep their indices: the cleaned stream is the same stream, 16 ms later.
 */
#define LE_VOICE_NS_RATE 16000
#define LE_VOICE_NS_FFT 256U
#define LE_VOICE_NS_HOP 128U
#define LE_VOICE_NS_BINS (LE_VOICE_NS_FFT / 2U + 1U)
#define LE_VOICE_NS_LATENCY_SAMPLES LE_VOICE_NS_FFT
/* Deepest cut applied to a bin judged to be noise only. */
#define LE_VOICE_NS_DEFAULT_ATTENUATION_DB 15
#define LE_VOICE_NS_MIN_ATTENUATION_DB 3
#define LE_VOICE_NS_MAX_ATTENUATION_DB 30
/* Minimum-statistics window: 8 sub-windows of 24 frames (8 ms) = 1.54 s. */
#define LE_VOICE_NS_SUBWINDOWS 8U
#define LE_VOICE_NS_SUBWINDOW_FRAMES 24U

struct le_voice_ns {
    float window[LE_VOICE_NS_FFT];
    float twiddle_re[LE_VOICE_NS_FFT / 2U];
    float twiddle_im[LE_VOICE_NS_FFT / 2U];
    float frame[LE_VOICE_NS_FFT];
    float overlap[LE_VOICE_NS_FFT];
    float smoothed[LE_VOICE_NS_BINS];
    float minimum[LE_VOICE_NS_BINS];
    float history[LE_VOICE_NS_SUBWINDOWS][LE_VOICE_NS_BINS];
    float previous[LE_VOICE_NS_BINS];
    int16_t input[LE_VOICE_NS_HOP];
    int16_t output[LE_VOICE_NS_HOP];
    unsigned short bit_reverse[LE_VOICE_NS_FFT];
    unsigned int position;
    unsigned int subwindow_frame;
    unsigned int subwindow_index;
    unsigned long long frames;
    float floor_gain;
};

/* Returns 0, or -1 for an attenuation outside the supported range. */
int le_voice_ns_init(struct le_voice_ns *ns, int attenuation_db);
/* Forget the noise estimate and the delay line; keeps the attenuation. */
void le_voice_ns_reset(struct le_voice_ns *ns);
void le_voice_ns_process(struct le_voice_ns *ns, int16_t *samples,
                         size_t count);

#endif
