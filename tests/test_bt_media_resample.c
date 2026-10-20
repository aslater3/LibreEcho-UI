/*
 * A2DP SBC media resampling contract.
 *
 * SBC decodes 128-frame blocks (16 blocks x 8 subbands).  btd used to resample
 * each block on its own with nearest-neighbour sampling, restarting the phase
 * every block: 44.1 kHz produced 140 frames per block instead of 139.32, so
 * the stream ran 0.49% fast and overflowed the media bus (dropped chunks), and
 * the per-block phase reset put -37 dBc sidebands at +/-344.5 Hz around every
 * tone (audible crackle).  This feeds SBC-sized blocks through the resampler
 * btd now uses and checks both the long-run rate and the frame-rate sidebands.
 */
#include "radio_resample.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SBC_BLOCK 128
#define OUT_MAX 512                       /* LE_MEDIA_CHUNK_FRAMES in btd */
#define ANALYSIS 8192

static int16_t stream[48000 * 2 * 3];

static double magnitude(const int16_t *x, int n, double hz)
{
    double re = 0.0, im = 0.0;
    int i;

    for (i = 0; i < n; ++i) {
        double w = 0.5 - 0.5 * cos(2.0 * M_PI * i / n);
        double a = 2.0 * M_PI * hz * i / 48000.0;

        re += w * x[i * 2] * cos(a);
        im -= w * x[i * 2] * sin(a);
    }
    return sqrt(re * re + im * im);
}

static int run(int rate, int seconds, long *produced)
{
    struct le_radio_resampler state;
    short block[SBC_BLOCK * 2];
    int16_t out[OUT_MAX * 2];
    long n = 0, total = 0;
    int blocks = rate * seconds / SBC_BLOCK, b, i, got;

    le_radio_resample_reset(&state);
    for (b = 0; b < blocks; ++b) {
        for (i = 0; i < SBC_BLOCK; ++i, ++n) {
            short v = (short)(12000.0 * sin(2.0 * M_PI * 1000.0 * n / rate));

            block[i * 2] = v;
            block[i * 2 + 1] = v;
        }
        got = le_radio_resample(&state, block, SBC_BLOCK, 2, rate, out,
                                OUT_MAX);
        if (got <= 0 || got >= OUT_MAX) {
            fprintf(stderr, "rate %d block %d produced %d frames\n", rate, b, got);
            return 1;
        }
        for (i = 0; i < got && total < (long)(sizeof(stream) / 4); ++i, ++total) {
            stream[total * 2] = out[i * 2];
            stream[total * 2 + 1] = out[i * 2 + 1];
        }
    }
    *produced = total;
    {
        double expected = (double)blocks * SBC_BLOCK * 48000.0 / rate;

        if (fabs((double)total - expected) > 4.0) {
            fprintf(stderr, "rate %d: %ld frames, expected %.1f (drift)\n",
                    rate, total, expected);
            return 1;
        }
    }
    return 0;
}

int main(void)
{
    static const int rates[] = { 16000, 32000, 44100, 48000 };
    long produced;
    size_t r;

    for (r = 0; r < sizeof(rates) / sizeof(rates[0]); ++r) {
        double fundamental, block_hz, worst = -200.0;
        const double offsets[] = { -1.0, 1.0, -2.0, 2.0 };
        size_t k;

        if (run(rates[r], 2, &produced))
            return 1;
        fundamental = magnitude(stream + 4800 * 2, ANALYSIS, 1000.0);
        block_hz = (double)rates[r] / SBC_BLOCK;
        for (k = 0; k < sizeof(offsets) / sizeof(offsets[0]); ++k) {
            double hz = 1000.0 + offsets[k] * block_hz;
            double db;

            if (hz <= 0.0 || hz >= 24000.0)
                continue;
            db = 20.0 * log10(magnitude(stream + 4800 * 2, ANALYSIS, hz) /
                              fundamental + 1e-12);
            if (db > worst)
                worst = db;
        }
        if (worst > -70.0) {
            fprintf(stderr, "rate %d: block-rate sideband %.1f dBc\n",
                    rates[r], worst);
            return 1;
        }
        printf("bt media resample %d Hz: %ld frames, sidebands %.1f dBc\n",
               rates[r], produced, worst);
    }
    puts("bt media resample: ok");
    return 0;
}
