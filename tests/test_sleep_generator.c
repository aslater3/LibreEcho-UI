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

/*
 * Reviewed defect (F1): a stop requested while the programme was still inside
 * its fade-in discarded the current gain and armed the stop ramp from 1.0, so
 * the first frame after the request jumped to full amplitude (the review probe
 * saw 751 -> 6863) before ramping down.  The stop must instead begin from the
 * gain in force at the request.  This stops 0.06 s into the 0.5 s fade-in.
 */
static void test_stop_continuity_inside_fade_in(void)
{
    struct le_sleep_gen g;
    double before, after;
    int prev, first_after, worst_step = 0;
    long produced = 0;
    size_t n, i;
    int guard = 0;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_HEARTBEAT, LE_SLEEP_BED_NONE, 60,
                            100, 0, 0, 13) == 0);
    CHECK(le_sleep_gen_fill(&g, block, 2880) == 2880);
    before = le_sleep_gain(&g, g.frames_done);
    CHECK(before > 0.0 && before < 0.2);             /* still fading in */
    prev = block[(2880 - 1) * LE_SLEEP_CHANNELS];

    le_sleep_gen_request_stop(&g);
    after = le_sleep_gain(&g, g.frames_done);
    /* The envelope is continuous across the request: no upward snap. */
    CHECK(fabs(after - before) <= 1.0 / (double)LE_SLEEP_FADE_OUT_FRAMES);

    CHECK(le_sleep_gen_fill(&g, block, 1) == 1);
    first_after = block[0];
    produced = 1;
    CHECK(abs(first_after - prev) < 1500);           /* no click at the edge */

    prev = first_after;
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
        CHECK(++guard < 1000);
    }
    CHECK(produced <= LE_SLEEP_FADE_OUT_FRAMES);     /* bounded */
    CHECK(worst_step < 1500);                        /* smooth all the way */
}

/*
 * Same rule when a stop interrupts a timer fade that is already running: the
 * stop ramp starts from the mid-fade gain (~0.5 here), never from 1.0.
 */
static void test_stop_continuity_inside_timer_fade(void)
{
    struct le_sleep_gen g;
    double before, after;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_HEARTBEAT, LE_SLEEP_BED_NONE, 60,
                            100, 10, 2, 17) == 0);
    while (g.frames_done < g.total_frames - g.fade_out_frames / 2) {
        size_t want = (size_t)(g.total_frames - g.fade_out_frames / 2 -
                               g.frames_done);
        size_t n;

        if (want > BLOCK)
            want = BLOCK;
        n = le_sleep_gen_fill(&g, block, want);
        CHECK(n == want);
    }
    CHECK(g.frames_done == g.total_frames - g.fade_out_frames / 2);
    before = le_sleep_gain(&g, g.frames_done);
    CHECK(before > 0.4 && before < 0.6);             /* mid timer fade */

    le_sleep_gen_request_stop(&g);
    after = le_sleep_gain(&g, g.frames_done);
    CHECK(fabs(after - before) <= 1.0 / (double)LE_SLEEP_FADE_OUT_FRAMES);
}

/* A repeated stop is a no-op and cannot extend the already-running ramp. */
static void test_stop_repeated_is_idempotent(void)
{
    struct le_sleep_gen g;
    int16_t last[LE_SLEEP_CHANNELS];
    long produced = 0;
    size_t n;
    int guard = 0;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_HEARTBEAT, LE_SLEEP_BED_NONE, 60,
                            100, 0, 0, 19) == 0);
    CHECK(le_sleep_gen_fill(&g, block, 2000) == 2000);
    le_sleep_gen_request_stop(&g);
    CHECK(g.stop_left == LE_SLEEP_FADE_OUT_FRAMES);
    le_sleep_gen_request_stop(&g);
    le_sleep_gen_request_stop(&g);
    CHECK(g.stop_left == LE_SLEEP_FADE_OUT_FRAMES);  /* not re-armed */
    CHECK(g.stopping == 1);

    for (;;) {
        n = le_sleep_gen_fill(&g, block, 4096);
        if (!n)
            break;
        produced += (long)n;
        memcpy(last, block + (n - 1) * LE_SLEEP_CHANNELS, sizeof(last));
        CHECK(++guard < 1000);
    }
    CHECK(produced == LE_SLEEP_FADE_OUT_FRAMES);     /* bounded, once */
    CHECK(last[0] == 0 && last[1] == 0);
}

/*
 * Regression (F1 follow-up): removing the initial upward snap is not enough.
 * When a stop arrives inside the fade-in, min() still lets the rising
 * programme term win until it crosses the descending stop ramp, so the applied
 * envelope keeps rising after the request.  A stop is a fade-*out*: from the
 * request onwards the gain must never exceed the gain in force at the request,
 * and must never rise, frame by frame.  This stops 0.06 s into the 0.5 s
 * fade-in, then walks the whole stop ramp one frame at a time.
 */
static void test_stop_envelope_never_rises(void)
{
    struct le_sleep_gen g;
    double at_stop, prev_gain, gain;
    long pos, span;
    int guard = 0;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_HEARTBEAT, LE_SLEEP_BED_NONE, 60,
                            100, 0, 0, 23) == 0);
    CHECK(le_sleep_gen_fill(&g, block, 2880) == 2880);   /* 0.06 s in */
    CHECK(g.frames_done == 2880);
    at_stop = le_sleep_gain(&g, g.frames_done);
    CHECK(at_stop > 0.0 && at_stop < 0.2);               /* still fading in */

    le_sleep_gen_request_stop(&g);
    CHECK(g.stop_start_gain == at_stop);                 /* gain captured */
    CHECK(g.stop_left == LE_SLEEP_FADE_OUT_FRAMES);

    prev_gain = at_stop;
    pos = g.frames_done;
    span = LE_SLEEP_FADE_OUT_FRAMES;
    while (pos < 2880 + span) {
        gain = le_sleep_gain(&g, pos);
        CHECK(gain <= at_stop + 1e-12);      /* never above the stop-time gain */
        CHECK(gain <= prev_gain + 1e-12);    /* never rises after the stop */
        prev_gain = gain;
        CHECK(le_sleep_gen_fill(&g, block, 1) == 1);
        ++pos;
        CHECK(++guard < 2 * LE_SLEEP_FADE_OUT_FRAMES);
    }
    CHECK(prev_gain < 1e-3);                 /* has faded towards silence */
}

/*
 * Same rule at the midpoint of a timer fade: the first stop snapshots the
 * mid-fade gain and arms the ramp; a repeated stop must not re-arm it or
 * re-read a gain that would rise again.
 */
static void test_stop_repeated_at_timer_midpoint(void)
{
    struct le_sleep_gen g;
    double stop_gain;

    CHECK(le_sleep_gen_init(&g, LE_SLEEP_SOURCE_HEARTBEAT, LE_SLEEP_BED_NONE, 60,
                            100, 10, 2, 29) == 0);
    while (g.frames_done < g.total_frames - g.fade_out_frames / 2) {
        size_t want = (size_t)(g.total_frames - g.fade_out_frames / 2 -
                               g.frames_done);
        size_t n;

        if (want > BLOCK)
            want = BLOCK;
        n = le_sleep_gen_fill(&g, block, want);
        CHECK(n == want);
    }
    le_sleep_gen_request_stop(&g);
    stop_gain = g.stop_start_gain;
    CHECK(stop_gain > 0.4 && stop_gain < 0.6);       /* mid timer fade */
    CHECK(g.stop_left == LE_SLEEP_FADE_OUT_FRAMES);

    CHECK(le_sleep_gen_fill(&g, block, 100) == 100);
    le_sleep_gen_request_stop(&g);                   /* repeated */
    CHECK(g.stop_left == LE_SLEEP_FADE_OUT_FRAMES - 100);  /* same ramp */
    CHECK(g.stop_start_gain == stop_gain);           /* gain not re-read */
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
    test_stop_continuity_inside_fade_in();
    test_stop_continuity_inside_timer_fade();
    test_stop_repeated_is_idempotent();
    test_stop_envelope_never_rises();
    test_stop_repeated_at_timer_midpoint();
    puts("sleep generator: cap, determinism, duration, fades, heartbeat "
         "envelope, validation, stop ramp and stop-from-current-gain: ok");
    return 0;
}
