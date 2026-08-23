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
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RATE 16000
#define CHANNELS 9
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

/* Ring of recent samples, so channel 3 can lag channel 0 by the array delay. */
#define HISTORY 64
static int16_t history[HISTORY];
static int history_pos;
static int array_delay = 4;      /* matches micd relative_delay_samples {0:4,3:0} */

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
    FILE *input = NULL;
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
    {
        const char *text = getenv("LE_FAKE_TINYCAP_ARRAY_DELAY");
        if (text && *text) {
            char *end = NULL;
            long parsed = strtol(text, &end, 10);
            if (end && *end == '\0' && parsed >= 0 && parsed < HISTORY)
                array_delay = (int)parsed;
        }
    }
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
            mono[i] = room_tone(tone_rms);
        if (input && !leading && got == 0) {
            /*
             * Source exhausted: keep the capture alive with room tone so
             * micd and waked stay up until they are signalled, exactly as a
             * real never-ending microphone array would.
             */
            fclose(input);
            input = NULL;
        }
        for (i = 0; i < CHUNK; ++i) {
            int16_t sample = (int16_t)(mono[i] / attenuate);
            int16_t lagged;

            /*
             * Model the array's propagation delay.
             *
             * micd beamforms logical mics 0 and 3 with
             * relative_delay_samples {0:4, 3:0}: it holds mic 0 back by four
             * samples so a wavefront that reached mic 0 first lines up with
             * mic 3 before they are summed. Feeding every channel the same
             * instant of audio does not model that -- it makes micd cancel a
             * delay that was never there, which turns delay-and-sum into a
             * comb filter with a null at rate/(2*4) = 2 kHz. Measured at
             * -13.6 dB, right in the consonant band, on every run.
             *
             * So delay mic 3 here by the same four samples. That is what a
             * real off-axis source does, micd's compensation then aligns the
             * two, and they add rather than cancel.
             */
            history[history_pos] = sample;
            lagged = history[(history_pos + HISTORY - array_delay) % HISTORY];
            history_pos = (history_pos + 1) % HISTORY;

            for (c = 0; c < CHANNELS; ++c)
                put24(frame + (size_t)(i * CHANNELS + c) * 3,
                      c == 3 ? lagged : sample);
        }
        if (fwrite(frame, 1, sizeof(frame), stdout) != sizeof(frame))
            return 0;
        fflush(stdout);
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
