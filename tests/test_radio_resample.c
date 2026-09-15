#include "adapter/radio_resample.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

#define BLOCK 1152
#define BLOCKS 120
#define BUS_RATE 48000

static int16_t output[BLOCKS * 2 * BLOCK * LE_RADIO_RESAMPLE_CHANNELS];

/*
 * Drive the resampler exactly as the decode loop does: one MPEG frame at a
 * time, with the state carried across calls.
 */
static int render(double hz, int rate)
{
    static short block[BLOCK * LE_RADIO_RESAMPLE_CHANNELS];
    struct le_radio_resampler state;
    long n = 0;
    int b, total = 0;

    le_radio_resample_reset(&state);
    for (b = 0; b < BLOCKS; ++b) {
        int i, produced;

        for (i = 0; i < BLOCK; ++i, ++n) {
            double v = sin(2.0 * M_PI * hz * (double)n / (double)rate) * 20000.0;

            block[i * 2] = (short)v;
            block[i * 2 + 1] = (short)v;
        }
        produced = le_radio_resample(&state, block, BLOCK, 2, rate,
                                     output + (long)total *
                                     LE_RADIO_RESAMPLE_CHANNELS,
                                     BLOCK * 2);
        CHECK(produced > 0);
        total += produced;
    }
    return total;
}

/*
 * Signal-to-everything-else for a pure tone, measured with a naive DFT at the
 * tone and at every other frequency that matters. A full FFT is not worth
 * carrying here; the tone frequencies are chosen to land on exact bins.
 */
static double tone_snr(int frames, double hz)
{
    const int n = 16384;
    const int offset = 5000;
    double signal = 0.0, total = 0.0;
    int k, i;

    if (frames < offset + n)
        return -1.0;
    for (i = 0; i < n; ++i) {
        double v = output[(offset + i) * LE_RADIO_RESAMPLE_CHANNELS];

        total += v * v;
    }
    k = (int)((hz / (double)BUS_RATE) * n + 0.5);
    for (i = -2; i <= 2; ++i) {
        double re = 0.0, im = 0.0;
        int j;

        for (j = 0; j < n; ++j) {
            double a = 2.0 * M_PI * (double)(k + i) * j / n;
            double v = output[(offset + j) * LE_RADIO_RESAMPLE_CHANNELS];

            re += v * cos(a);
            im -= v * sin(a);
        }
        signal += 2.0 * (re * re + im * im) / n;
    }
    if (total <= signal)
        return 1000.0;
    return 10.0 * log10(signal / (total - signal));
}

int main(void)
{
    struct le_radio_resampler state;
    static short block[BLOCK * LE_RADIO_RESAMPLE_CHANNELS];
    int frames, i, produced;
    double snr;

    /*
     * A 48 kHz source must come through untouched. Ratio 1.0 puts every
     * output sample on an input sample with a zero fraction, so anything but
     * an exact copy means the phase drifted.
     */
    frames = render(6000.0, 48000);
    CHECK(frames > 0);
    for (i = 0; i < 100000; ++i) {
        double want = sin(2.0 * M_PI * 6000.0 * (double)i / 48000.0) * 20000.0;
        short got = output[(i + 1) * LE_RADIO_RESAMPLE_CHANNELS];

        CHECK(got == (short)want);
    }

    /*
     * 44.1 kHz is the case that regressed: restarting the phase each block
     * measured -47 dB here, worse than the tone itself. These floors are set
     * well below the measured 41 dB so ordinary arithmetic differences do not
     * trip them, but a phase reset cannot pass.
     */
    frames = render(6000.0, 44100);
    snr = tone_snr(frames, 6000.0);
    fprintf(stderr, "44.1 kHz 6 kHz SNR %.1f dB\n", snr);
    CHECK(snr > 30.0);

    frames = render(3000.0, 44100);
    snr = tone_snr(frames, 3000.0);
    fprintf(stderr, "44.1 kHz 3 kHz SNR %.1f dB\n", snr);
    CHECK(snr > 45.0);

    /*
     * No input frame may be dropped or repeated. At 44.1 kHz the output is
     * 48000/44100 of the input, and losing one frame per block was the bug.
     */
    frames = render(1000.0, 44100);
    CHECK(frames > (int)(BLOCKS * BLOCK * 48000.0 / 44100.0) - 8);
    CHECK(frames < (int)(BLOCKS * BLOCK * 48000.0 / 44100.0) + 8);

    /* A mono block must fold to both channels. */
    le_radio_resample_reset(&state);
    for (i = 0; i < BLOCK; ++i)
        block[i] = (short)(i * 3);
    produced = le_radio_resample(&state, block, BLOCK, 1, 48000,
                                 output, BLOCK * 2);
    CHECK(produced > 0);
    for (i = 0; i < produced; ++i)
        CHECK(output[i * LE_RADIO_RESAMPLE_CHANNELS] ==
              output[i * LE_RADIO_RESAMPLE_CHANNELS + 1]);

    /* Degenerate arguments must be refused rather than trusted. */
    CHECK(le_radio_resample(&state, block, 0, 2, 48000, output, 16) == 0);
    CHECK(le_radio_resample(&state, block, BLOCK, 2, 0, output, 16) == 0);
    CHECK(le_radio_resample(&state, block, BLOCK, 2, 48000, output, 0) == 0);

    printf("test_radio_resample OK\n");
    return 0;
}
