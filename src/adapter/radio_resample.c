#include "radio_resample.h"

#include <string.h>

#define BUS_RATE 48000
#define CHANNELS LE_RADIO_RESAMPLE_CHANNELS
#define HISTORY LE_RADIO_RESAMPLE_HISTORY
#define MAX_IN 1152                       /* an MPEG-1 Layer III frame */

void le_radio_resample_reset(struct le_radio_resampler *state)
{
    memset(state, 0, sizeof(*state));
}

/*
 * Catmull-Rom. It costs a few multiplies over linear interpolation and buys
 * about 11 dB of SNR at 6 kHz on a 44.1 kHz source, which is worth having on
 * a 320 kbps stream.
 */
static short interpolate(short p0, short p1, short p2, short p3, double t)
{
    double v = p1 + 0.5 * t * (p2 - p0 +
               t * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 +
               t * (3.0 * (p1 - p2) + p3 - p0)));

    if (v > 32767.0)
        v = 32767.0;
    if (v < -32768.0)
        v = -32768.0;
    return (short)v;
}

int le_radio_resample(struct le_radio_resampler *state, const short *pcm,
                      int frames, int channels, int rate,
                      int16_t *output, int output_frames)
{
    static short in[(HISTORY + MAX_IN) * CHANNELS];
    double ratio, next;
    int total, i, produced = 0;

    if (!state || !pcm || !output || frames <= 0 || rate <= 0 ||
        output_frames <= 0)
        return 0;
    if (frames > MAX_IN)
        frames = MAX_IN;
    if (rate != state->rate) {            /* a stream that changed shape */
        le_radio_resample_reset(state);
        state->rate = rate;
    }
    ratio = (double)BUS_RATE / (double)rate;

    if (!state->primed) {
        /* Seed the history with the first frame so a stream does not open on
           a step up from silence. */
        for (i = 0; i < HISTORY; ++i) {
            state->history[i * CHANNELS] = pcm[0];
            state->history[i * CHANNELS + 1] = channels >= 2 ? pcm[1] : pcm[0];
        }
        state->phase = HISTORY - 1;
        state->primed = 1;
    }

    memcpy(in, state->history, sizeof(state->history));
    for (i = 0; i < frames; ++i) {
        in[(HISTORY + i) * CHANNELS] = channels >= 2 ? pcm[i * 2] : pcm[i];
        in[(HISTORY + i) * CHANNELS + 1] =
            channels >= 2 ? pcm[i * 2 + 1] : pcm[i];
    }
    total = HISTORY + frames;

    /* Emit while the p2/p3 neighbours are still inside the buffer. */
    for (;;) {
        double position = state->phase + (double)produced / ratio;
        int index = (int)position;
        double t = position - index;

        if (index + 2 >= total || produced >= output_frames)
            break;
        output[produced * CHANNELS] =
            interpolate(in[(index - 1) * CHANNELS], in[index * CHANNELS],
                        in[(index + 1) * CHANNELS],
                        in[(index + 2) * CHANNELS], t);
        output[produced * CHANNELS + 1] =
            interpolate(in[(index - 1) * CHANNELS + 1],
                        in[index * CHANNELS + 1],
                        in[(index + 1) * CHANNELS + 1],
                        in[(index + 2) * CHANNELS + 1], t);
        ++produced;
    }

    /*
     * Carry the tail of the buffer, not the frames around the next output
     * position: the loop stops two frames short of the end and those two are
     * still unconsumed input. Anchoring the carry on the output position
     * discarded them, losing exactly one input frame per decoded MP3 frame.
     */
    next = state->phase + (double)produced / ratio;
    for (i = 0; i < HISTORY; ++i) {
        int src = total - HISTORY + i;

        state->history[i * CHANNELS] = in[src * CHANNELS];
        state->history[i * CHANNELS + 1] = in[src * CHANNELS + 1];
    }
    state->phase = next - (double)(total - HISTORY);
    return produced;
}
