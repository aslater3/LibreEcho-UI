/*
 * Sample-rate conversion for the internet radio player.
 *
 * Split out of radiod.c for the same reason voice_dsp.c is split out of
 * micd.c: the arithmetic is worth testing on its own, and radiod.c pulls in
 * minimp3, which a strict test build will not tolerate.
 */
#ifndef LIBREECHO_RADIO_RESAMPLE_H
#define LIBREECHO_RADIO_RESAMPLE_H

#include <stdint.h>

#define LE_RADIO_RESAMPLE_HISTORY 3
#define LE_RADIO_RESAMPLE_CHANNELS 2

struct le_radio_resampler {
    short history[LE_RADIO_RESAMPLE_HISTORY * LE_RADIO_RESAMPLE_CHANNELS];
    double phase;
    int rate;
    int primed;
};

void le_radio_resample_reset(struct le_radio_resampler *state);

/*
 * Convert one decoded block to interleaved 48 kHz stereo. Returns the number
 * of frames written to output, which is never more than output_frames.
 *
 * The state must persist across calls for a given stream: the phase and the
 * trailing input frames carry over, and resetting them per block is audible.
 */
int le_radio_resample(struct le_radio_resampler *state, const short *pcm,
                      int frames, int channels, int rate,
                      int16_t *output, int output_frames);

#endif
