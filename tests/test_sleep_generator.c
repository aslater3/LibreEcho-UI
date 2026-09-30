/*
 * Sleep-source synthesis: production PCM behaviour.
 *
 * Exercises the shipped generator header directly (no audiod, no ALSA, no
 * device): amplitude cap, level/tempo validation clamps, deterministic output,
 * exact finite duration, fade-in, pre-timer fade-out, the heartbeat's smooth
 * double pulse, the optional bed, and the bounded stop ramp.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "../src/adapter/sleep_generator.h"

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        exit(1); \
    } \
} while (0)

#define BLOCK 4096

static int16_t block[BLOCK * LE_SLEEP_CHANNELS];

static int max_abs(const int16_t *pcm, size_t frames)
{
    int worst = 0;
    size_t i;

    for (i = 0; i < frames * LE_SLEEP_CHANNELS; ++i) {
        int v = pcm[i] < 0 ? -pcm[i] : pcm[i];

        if (v > worst)
            worst = v;
    }
    return worst;
}

/* The generator clamps level and never exceeds the hard output ceiling. */
static void test_amplitude_cap(void)
{
    struct le_sleep_gen g;
    int worst = 0;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_WHITE, LE_SLEEP_BED_NONE, 60,
                            100000, 0, 0, 1) == 0);   /* absurd level */
    CHECK(g.level == LE_SLEEP_LEVEL_MAX);
    {
        size_t n = le_sleep_gen_fill(&g, block, BLOCK);

        CHECK(n == BLOCK);
        worst = max_abs(block, n);
    }
    CHECK(worst <= LE_SLEEP_AMPLITUDE_CAP);
    CHECK(worst > 0);

    /* And the heartbeat + bed path cannot exceed it either. */
    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_HEARTBEAT, LE_SLEEP_BED_BROWN,
                            100, 100, 0, 0, 7) == 0);
    {
        int peak = 0, k;

        for (k = 0; k < 40; ++k) {
            size_t n = le_sleep_gen_fill(&g, block, BLOCK);

            if (!n)
                break;
            if (max_abs(block, n) > peak)
                peak = max_abs(block, n);
        }
        CHECK(peak <= LE_SLEEP_AMPLITUDE_CAP);
        CHECK(peak > 0);
    }
}

/* Same seed, same bytes; a different seed changes the noise stream. */
static void test_determinism(void)
{
    struct le_sleep_gen a, b, c;
    int16_t out_a[512 * LE_SLEEP_CHANNELS], out_b[512 * LE_SLEEP_CHANNELS];
    int16_t out_c[512 * LE_SLEEP_CHANNELS];

    CHECK(le_sleep_gen_init(&a, LE_SLEEP_SOURCE_PINK, LE_SLEEP_BED_NONE, 60, 50,
                            0, 0, 0xABCDEF01u) == 0);
    CHECK(le_sleep_gen_init(&b, LE_SLEEP_SOURCE_PINK, LE_SLEEP_BED_NONE, 60, 50,
                            0, 0, 0xABCDEF01u) == 0);
    CHECK(le_sleep_gen_init(&c, LE_SLEEP_SOURCE_PINK, LE_SLEEP_BED_NONE, 60, 50,
                            0, 0, 0x00000002u) == 0);
    CHECK(le_sleep_gen_fill(&a, out_a, 512) == 512);
    CHECK(le_sleep_gen_fill(&b, out_b, 512) == 512);
    CHECK(le_sleep_gen_fill(&c, out_c, 512) == 512);
    CHECK(memcmp(out_a, out_b, sizeof(out_a)) == 0);
    CHECK(memcmp(out_a, out_c, sizeof(out_a)) != 0);
}

/* A finite request is exact: seconds * 48000 frames, then silence. */
static void test_duration(void)
{
    struct le_sleep_gen g;
    long total = 0;
    size_t n;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_WHITE, LE_SLEEP_BED_NONE, 60, 40,
                            2, 0, 3) == 0);
    CHECK(le_sleep_gen_remaining(&g) == 2 * LE_SLEEP_RATE);
    while ((n = le_sleep_gen_fill(&g, block, BLOCK)) > 0)
        total += (long)n;
    CHECK(total == 2 * LE_SLEEP_RATE);
    CHECK(le_sleep_gen_remaining(&g) == 0);
    CHECK(le_sleep_gen_fill(&g, block, BLOCK) == 0);
}

/* Fade-in starts at zero and reaches full level by LE_SLEEP_FADE_IN_FRAMES. */
static void test_fade_in(void)
{
    struct le_sleep_gen g;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_WHITE, LE_SLEEP_BED_NONE, 60, 100,
                            0, 0, 5) == 0);
    CHECK(le_sleep_gen_fill(&g, block, 1) == 1);
    CHECK(block[0] == 0 && block[1] == 0);           /* gain 0 at frame 0 */
    {
        /* Skip to the end of the fade and confirm full amplitude coverage. */
        int peak = 0, k;

        for (k = 0; k < 8; ++k) {
            size_t n = le_sleep_gen_fill(&g, block, 4000);

            if (max_abs(block, n) > peak)
                peak = max_abs(block, n);
        }
        CHECK(g.frames_done >= LE_SLEEP_FADE_IN_FRAMES);
        CHECK(peak > LE_SLEEP_AMPLITUDE_CAP / 2);
    }
}

/* The pre-timer fade reaches zero at the last frame, and is bounded to it. */
static void test_fade_out_bounded_to_timer(void)
{
    struct le_sleep_gen g;
    int16_t last[LE_SLEEP_CHANNELS];
    size_t n;
    int guard = 0;

    /* Ask for a one-hour fade on a two-second timer: it must bound to 2 s. */
    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_WHITE, LE_SLEEP_BED_NONE, 60, 60,
                            2, 3600, 9) == 0);
    CHECK(g.fade_out_frames == 2 * LE_SLEEP_RATE);
    for (;;) {
        n = le_sleep_gen_fill(&g, block, BLOCK);
        if (!n)
            break;
        memcpy(last, block + (n - 1) * LE_SLEEP_CHANNELS, sizeof(last));
        CHECK(++guard < 100000);
    }
    CHECK(last[0] == 0 && last[1] == 0);
}

/*
 * The heartbeat is a smooth low-frequency double pulse: it must never step
 * abruptly (no click), and within one beat it must carry energy in the two
 * pulse windows and near-silence in the gap between beats.
 */
static void test_heartbeat_envelope(void)
{
    struct le_sleep_gen g;
    static int16_t buf[2 * LE_SLEEP_RATE * LE_SLEEP_CHANNELS];
    int prev = 0, worst_step = 0;
    size_t i;
    long lub = 0, dub = 0, gap = 0;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_HEARTBEAT, LE_SLEEP_BED_NONE, 60,
                            100, 0, 0, 11) == 0);
    CHECK(le_sleep_gen_fill(&g, buf, 2 * LE_SLEEP_RATE) == 2 * LE_SLEEP_RATE);
    for (i = 0; i < 2 * LE_SLEEP_RATE * LE_SLEEP_CHANNELS;
         i += LE_SLEEP_CHANNELS) {
        int v = buf[i];

        if (i && abs(v - prev) > worst_step)
            worst_step = abs(v - prev);
        prev = v;
    }
    CHECK(worst_step < 1500);                        /* smooth, not a click */
    /* Measure the second beat: after the fade-in the amplitudes are honest. */
    for (i = LE_SLEEP_RATE; i < 2 * (size_t)LE_SLEEP_RATE; ++i) {
        int v = abs(buf[i * LE_SLEEP_CHANNELS]);
        double s = (double)(i - LE_SLEEP_RATE) / (double)LE_SLEEP_RATE;

        if (s < 0.10)
            lub += v;
        else if (s >= 0.30 && s < 0.385)
            dub += v;
        else if (s >= 0.45 && s < 0.95)
            gap += v;
    }
    CHECK(lub > 0 && dub > 0);
    CHECK(dub < lub);                                /* "dub" is softer */
    CHECK(gap == 0);                                 /* silence between beats */
}

/* Tempo and bed are clamped/limited, and no source means no generator. */
static void test_input_validation(void)
{
    struct le_sleep_gen g;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_NONE, LE_SLEEP_BED_NONE, 60, 50,
                            0, 0, 1) == -1);
    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_HEARTBEAT, LE_SLEEP_BED_PINK,
                            1, 50, 0, 0, 1) == 0);
    CHECK(g.tempo_bpm == LE_SLEEP_TEMPO_MIN);
    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_HEARTBEAT, LE_SLEEP_BED_PINK,
                            1000, 50, 0, 0, 1) == 0);
    CHECK(g.tempo_bpm == LE_SLEEP_TEMPO_MAX);
    CHECK(g.bed == LE_SLEEP_BED_PINK);
    CHECK(g.fade_out_frames == 0);                   /* unlimited stays unfaded */
    /* The bed is dropped for the plain noise sources. */
    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_BROWN, LE_SLEEP_BED_BROWN, 60, 50,
                            0, 0, 1) == 0);
    CHECK(g.bed == LE_SLEEP_BED_NONE);
}

/* The heartbeat + bed combination is real audio, not a silent path. */
static void test_heartbeat_with_bed(void)
{
    struct le_sleep_gen g;
    int peak = 0, k;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_HEARTBEAT, LE_SLEEP_BED_BROWN, 60,
                            70, 0, 0, 21) == 0);
    for (k = 0; k < 20; ++k) {
        size_t n = le_sleep_gen_fill(&g, block, BLOCK);

        if (!n)
            break;
        if (max_abs(block, n) > peak)
            peak = max_abs(block, n);
    }
    CHECK(peak > 0);
}

/* An explicit stop ramps to silence in a bounded number of frames. */
static void test_stop_ramp(void)
{
    struct le_sleep_gen g;
    int16_t last[LE_SLEEP_CHANNELS];
    long produced = 0;
    int prev = 0, worst_step = 0;
    size_t n, i;
    int guard = 0;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_HEARTBEAT, LE_SLEEP_BED_NONE, 60,
                            100, 0, 0, 13) == 0);
    CHECK(le_sleep_gen_fill(&g, block, 4000) == 4000);
    prev = block[(4000 - 1) * LE_SLEEP_CHANNELS];
    le_sleep_gen_request_stop(&g);
    for (;;) {
        n = le_sleep_gen_fill(&g, block, 1024);
        if (!n)
            break;
        produced += (long)n;
        for (i = 0; i < n; ++i) {
            int v = block[i * LE_SLEEP_CHANNELS];

            if (abs(v - prev) > worst_step)
                worst_step = abs(v - prev);
            prev = v;
        }
        memcpy(last, block + (n - 1) * LE_SLEEP_CHANNELS, sizeof(last));
        CHECK(++guard < 1000);
    }
    CHECK(produced <= LE_SLEEP_FADE_OUT_FRAMES);
    CHECK(produced > 0);
    CHECK(last[0] == 0 && last[1] == 0);             /* ends in silence */
    CHECK(worst_step < 1500);                        /* and smoothly */
}

int main(void)
{
    test_amplitude_cap();
    test_determinism();
    test_duration();
    test_fade_in();
    test_fade_out_bounded_to_timer();
    test_heartbeat_envelope();
    test_input_validation();
    test_heartbeat_with_bed();
    test_stop_ramp();
    puts("sleep generator: cap, determinism, duration, fades, heartbeat "
         "envelope, validation and stop ramp: ok");
    return 0;
}
