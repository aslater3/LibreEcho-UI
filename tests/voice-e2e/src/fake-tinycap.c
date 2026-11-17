/*
 * Stand-in for the device's pinned tinycap.
 *
 * micd execs it as:
 *   tinycap -- -D 0 -d 24 -c 9 -r 16000 -b 24 -p 640 -n 4
 * and then reads packed S24_3LE straight off stdout with no WAV header.
 *
 * We reproduce exactly that: 9 interleaved channels of 3-byte little-endian
 * samples in which the 16 valid bits are LEFT-JUSTIFIED (word = s16 << 8),
 * which is the alignment voice_dsp.h documents for the MT8163 transport.
 * Getting this wrong is the bug the pipeline already had once, so the flags
 * are checked rather than ignored -- if micd ever asks for a different
 * geometry the harness fails loudly instead of feeding it mismatched audio.
 *
 * Output is paced at real time so the downstream wake worker, VAD hangover
 * and STT endpointing see the cadence they see on hardware.
 *
 * The seven microphone lanes are rendered from the measured array layout in
 * src/adapter/mic_array_geometry.h: the fixture is a far-field talker at
 * LE_FAKE_TINYCAP_AZIMUTH degrees (0 = front, clockwise from above), and each
 * lane receives it with that microphone's fractional propagation delay.  An
 * optional second file, LE_FAKE_TINYCAP_NOISE_SOURCE, is rendered the same way
 * from LE_FAKE_TINYCAP_NOISE_AZIMUTH and scaled by LE_FAKE_TINYCAP_NOISE_GAIN
 * (percent).  Room tone is independent per lane, like microphone self-noise.
 * The capture path itself adds no lane skew; that was measured on hardware.
 *
 * LE_FAKE_TINYCAP_OFFLINE=1 drops the real-time pacing and exits once the
 * fixture is exhausted, for offline evaluation (bin/compare-beam-mics.py).
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "adapter/mic_array_render.h"

#define RATE 16000
#define CHANNELS 9
#define MICS LE_ARRAY_RENDER_MICS
#define CHUNK 160          /* 10 ms */

static int flag_value(int argc, char **argv, const char *flag, int fallback)
{
    int i;

    for (i = 1; i + 1 < argc; ++i)
        if (!strcmp(argv[i], flag))
            return atoi(argv[i + 1]);
    return fallback;
}

static void put24(unsigned char *p, int16_t sample)
{
    int32_t word = (int32_t)sample << 8;   /* left-justified 16 valid bits */

    p[0] = (unsigned char)(word & 0xff);
    p[1] = (unsigned char)((word >> 8) & 0xff);
    p[2] = (unsigned char)((word >> 16) & 0xff);
}

static uint32_t rng_state = 0x1234abcdU;

static int16_t room_tone(int rms)
{
    /* Cheap triangular noise; the array is never digitally silent. */
    int32_t a, b;

    rng_state = rng_state * 1103515245U + 12345U;
    a = (int32_t)((rng_state >> 16) & 0x7fff);
    rng_state = rng_state * 1103515245U + 12345U;
    b = (int32_t)((rng_state >> 16) & 0x7fff);
    return (int16_t)(((a - b) * rms) / 16384);
}

struct source {
    FILE *file;
    struct le_array_render render;
};

static double env_double(const char *name, double fallback)
{
    const char *text = getenv(name);
    char *end = NULL;
    double value;

    if (!text || !*text)
        return fallback;
    value = strtod(text, &end);
    return end && *end == '\0' ? value : fallback;
}

static int16_t clamp16(float v)
{
    if (v > 32767.0f)
        return 32767;
    if (v < -32768.0f)
        return -32768;
    return (int16_t)lrintf(v);
}

int main(int argc, char **argv)
{
    const char *source = getenv("LE_FAKE_TINYCAP_SOURCE");
    const char *attenuate_text = getenv("LE_FAKE_TINYCAP_ATTENUATE");
    const char *tone_text = getenv("LE_FAKE_TINYCAP_ROOM_TONE_RMS");
    const char *lead_text = getenv("LE_FAKE_TINYCAP_LEAD_MS");
    int attenuate = attenuate_text && *attenuate_text
        ? atoi(attenuate_text) : 1;
    int tone_rms = tone_text && *tone_text ? atoi(tone_text) : 14;
    /*
     * Room tone emitted before the fixture starts.  waked loads three ONNX
     * graphs after it opens the microphone stream; without a lead the phrase
     * would be half spoken before the classifier is warm, and the harness
     * would report a wake failure that the device does not have.
     */
    long lead_chunks = (lead_text && *lead_text ? atol(lead_text) : 1500) / 10;
    static struct source talker, noise;
    FILE *input = NULL;
    const char *noise_path = getenv("LE_FAKE_TINYCAP_NOISE_SOURCE");
    double noise_gain = env_double("LE_FAKE_TINYCAP_NOISE_GAIN", 100.0) / 100.0;
    const char *offline_text = getenv("LE_FAKE_TINYCAP_OFFLINE");
    int offline = offline_text && !strcmp(offline_text, "1");
    struct timespec next;
    int channels = flag_value(argc, argv, "-c", 0);
    int rate = flag_value(argc, argv, "-r", 0);
    int bits = flag_value(argc, argv, "-b", 0);

    if (channels != CHANNELS || rate != RATE || bits != 24) {
        fprintf(stderr,
                "fake-tinycap: micd asked for c=%d r=%d b=%d, "
                "harness only models 9ch/16000/24\n",
                channels, rate, bits);
        return 2;
    }
    if (attenuate < 1)
        attenuate = 1;
    if (source && *source) {
        input = fopen(source, "rb");
        if (!input) {
            fprintf(stderr, "fake-tinycap: cannot open %s: %s\n",
                    source, strerror(errno));
            return 2;
        }
    }
    if (noise_path && *noise_path) {
        noise.file = fopen(noise_path, "rb");
        if (!noise.file) {
            fprintf(stderr, "fake-tinycap: cannot open %s: %s\n",
                    noise_path, strerror(errno));
            return 2;
        }
    }
    le_array_render_init(&talker.render,
                         env_double("LE_FAKE_TINYCAP_AZIMUTH", 0.0));
    le_array_render_init(&noise.render,
                         env_double("LE_FAKE_TINYCAP_NOISE_AZIMUTH", 90.0));
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        unsigned char frame[CHUNK * CHANNELS * 3];
        int16_t mono[CHUNK];
        size_t got = 0;
        int leading = lead_chunks > 0;
        int i, c;

        if (leading)
            --lead_chunks;
        else if (input)
            got = fread(mono, sizeof(int16_t), CHUNK, input);
        for (i = (int)got; i < CHUNK; ++i)
            mono[i] = 0;   /* per-lane room tone is added below */
        if (input && !leading && got == 0) {
            /*
             * Source exhausted: keep the capture alive with room tone so
             * micd and waked stay up until they are signalled, exactly as a
             * real never-ending microphone array would.
             */
            fclose(input);
            input = NULL;
            if (offline)
                return 0;
        }
        for (i = 0; i < CHUNK; ++i) {
            float noise_sample = 0.0f;

            if (noise.file) {
                int16_t n;
                if (fread(&n, sizeof(n), 1, noise.file) != 1) {
                    rewind(noise.file);
                    if (fread(&n, sizeof(n), 1, noise.file) != 1)
                        n = 0;
                }
                noise_sample = (float)(n * noise_gain);
            }
            le_array_render_push(&talker.render, (float)(mono[i] / attenuate));
            le_array_render_push(&noise.render, noise_sample);
            for (c = 0; c < CHANNELS; ++c) {
                float v = (float)room_tone(tone_rms);
                if (c < MICS)
                    v += le_array_render_lane(&talker.render, (unsigned int)c) +
                         le_array_render_lane(&noise.render, (unsigned int)c);
                put24(frame + (size_t)(i * CHANNELS + c) * 3, clamp16(v));
            }
        }
        if (fwrite(frame, 1, sizeof(frame), stdout) != sizeof(frame))
            return 0;
        fflush(stdout);
        if (offline)
            continue;
        next.tv_nsec += 10000000L;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            ++next.tv_sec;
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL)
               == EINTR)
            ;
    }
}
