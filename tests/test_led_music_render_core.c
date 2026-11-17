/*
 * Focused behavioural tests for the twelve-pixel music renderer (#64, #65).
 *
 * Build:
 *   gcc -std=c99 -Wall -Wextra -Wpedantic -O2 -o build/test-led-music-render \
 *       tests/test_led_music_render_core.c src/adapter/led_music_render.c \
 *       src/adapter/led_music_director.c src/adapter/led_output.c \
 *       src/adapter/music_visualizer_protocol.c
 */
#include "../src/adapter/led_music_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "render: %s\n", message);
        exit(1);
    }
}

static struct le_music_features feat(void)
{
    struct le_music_features f;
    memset(&f, 0, sizeof(f));
    f.feature_version = 2U;
    f.session = 1U;
    f.timestamp_ms = 1000U;
    f.energy = 170U;
    f.density = 120U;
    f.warmth = 90U;
    f.transientness = 90U;
    f.loudness_fast = 140U;
    f.loudness_slow = 150U;
    f.onset_low = 180U;
    f.onset_mid = 120U;
    f.onset_high = 90U;
    f.beat_confidence = 200U;
    f.beat_phase = 20000U;
    return f;
}

static struct le_music_scene scene_for(enum le_music_grammar grammar,
                                       struct led_rgb accent)
{
    struct le_music_scene scene;
    memset(&scene, 0, sizeof(scene));
    scene.grammar = grammar;
    scene.next_grammar = grammar;
    scene.palette.primary = (struct led_rgb){40U, 120U, 220U};
    scene.palette.secondary = (struct led_rgb){20U, 200U, 190U};
    scene.palette.accent = accent;
    scene.next_palette = scene.palette;
    return scene;
}

static unsigned long signature(const struct led_rgb out[LE_LED_PIXELS])
{
    unsigned long h = 2166136261UL;
    int i;
    for (i = 0; i < LE_LED_PIXELS; i++) {
        h = (h ^ out[i].r) * 16777619UL;
        h = (h ^ out[i].g) * 16777619UL;
        h = (h ^ out[i].b) * 16777619UL;
    }
    return h;
}

static int lit_pixels(const struct led_rgb out[LE_LED_PIXELS])
{
    int count = 0, i;
    for (i = 0; i < LE_LED_PIXELS; i++)
        if (out[i].r || out[i].g || out[i].b)
            count++;
    return count;
}

static int exact_matches(const struct led_rgb out[LE_LED_PIXELS],
                         struct led_rgb colour)
{
    int count = 0, i;
    for (i = 0; i < LE_LED_PIXELS; i++)
        if (out[i].r == colour.r && out[i].g == colour.g &&
            out[i].b == colour.b)
            count++;
    return count;
}

static unsigned int total_light(const struct led_rgb out[LE_LED_PIXELS])
{
    unsigned int sum = 0U;
    int i;
    for (i = 0; i < LE_LED_PIXELS; i++)
        sum += out[i].r + out[i].g + out[i].b;
    return sum;
}

static unsigned int le_distance_px(int a, int b)
{
    int d = a > b ? a - b : b - a;
    return (unsigned int)(d > LE_LED_PIXELS / 2 ? LE_LED_PIXELS - d : d);
}

static int brightest(const struct led_rgb out[LE_LED_PIXELS])
{
    unsigned int best = 0U;
    int at = 0, i;
    for (i = 0; i < LE_LED_PIXELS; i++) {
        unsigned int v = out[i].r + out[i].g + out[i].b;
        if (v > best) {
            best = v;
            at = i;
        }
    }
    return at;
}

/* Runs of lit pixels round the ring (0 when dark, 1 when all are lit). */
static int lit_groups(const struct led_rgb out[LE_LED_PIXELS])
{
    int groups = 0, i;
    for (i = 0; i < LE_LED_PIXELS; i++) {
        int prev = (i + LE_LED_PIXELS - 1) % LE_LED_PIXELS;
        int lit = out[i].r || out[i].g || out[i].b;
        int before = out[prev].r || out[prev].g || out[prev].b;
        if (lit && !before)
            groups++;
    }
    return groups == 0 && lit_pixels(out) > 0 ? 1 : groups;
}

static int in_range(const struct led_rgb out[LE_LED_PIXELS])
{
    int i;
    for (i = 0; i < LE_LED_PIXELS; i++)
        if (out[i].r > 255U || out[i].g > 255U || out[i].b > 255U)
            return 0;
    return 1;
}

/* Ring travel between two motion positions, in 1/65536 px (the position
   wraps at two revolutions). */
static unsigned int travel_q16(unsigned int from, unsigned int to)
{
    unsigned int wrap = 2U * (unsigned int)LE_LED_PIXELS * 65536U;
    return (to + wrap - from) % wrap;
}

/* What the music is doing in a motion trace. */
enum trace_kind {
    TRACE_BEAT,       /* 120 bpm locked, a kick on the beat, snare off it */
    TRACE_FREE,       /* steady sound, no beat lock and no hits */
    TRACE_SILENCE     /* nothing */
};

struct motion_report {
    unsigned int windows;          /* 0.5 s windows observed */
    unsigned int static_windows;   /* windows whose frames were all equal */
    unsigned int least_distinct;   /* fewest different frames in a window */
    unsigned int travel;           /* 1/65536 px */
    unsigned int kicks;            /* rises of the kick envelope */
    unsigned int peak_light;       /* brightest frame, summed channels */
    unsigned int low_light;        /* dimmest frame */
    int max_lit;
    int min_lit;
    int ranged;                    /* every channel stayed within 0..255 */
};

/*
 * Run ledd timing for `ms` milliseconds: a producer frame every 85.4 ms and a
 * render every 33 ms on the frame timeline, as apply_music_v2() does.
 */
static void run_motion(enum le_music_grammar grammar, struct led_rgb accent,
                       enum trace_kind kind, unsigned int ms,
                       struct motion_report *report)
{
    struct le_music_scene scene = scene_for(grammar, accent);
    struct le_music_render_state state;
    struct le_music_features f = feat();
    struct led_rgb out[LE_LED_PIXELS];
    struct led_rgb seen[16][LE_LED_PIXELS];
    unsigned int distinct = 0U, next_x10 = 0U, seq = 0U, t, k;
    unsigned int last_motion, prev_env = 0U;

    memset(report, 0, sizeof(*report));
    report->least_distinct = 1000U;
    report->low_light = 1000000U;
    report->ranged = 1;
    report->min_lit = LE_LED_PIXELS;
    le_music_render_state_reset(&state, 1U);
    last_motion = state.motion_q16;
    for (t = 0U; t < ms; t++) {
        int fresh = 0;
        if (t * 10U >= next_x10) {
            unsigned int beat_ms = t % 500U;
            f = feat();
            f.onset_low = f.onset_mid = f.onset_high = 0U;
            if (kind == TRACE_BEAT) {
                f.beat_confidence = 220U;
                f.bpm_x100 = 12000U;
                f.beat_strength = 200U;
                f.beat_phase = beat_ms * 65536U / 500U;
                if (beat_ms < 90U) {
                    f.onset_low = 230U;
                    f.events |= LE_MUSIC_EVENT_KICK;
                }
                if (beat_ms >= 250U && beat_ms < 340U) {
                    f.onset_mid = 200U;
                    f.onset_high = 160U;
                    f.events |= LE_MUSIC_EVENT_SNARE;
                }
            } else {
                f.beat_confidence = 30U;
                f.bpm_x100 = 0U;
                f.beat_phase = 0U;
                if (kind == TRACE_SILENCE) {
                    f.energy = 0U;
                    f.density = 0U;
                    f.loudness_fast = 0U;
                    f.loudness_slow = 0U;
                }
            }
            f.seq = ++seq;
            f.timestamp_ms = 1000U + t;
            next_x10 += 854U;
            fresh = 1;
        }
        if (fresh || t % 33U == 0U) {
            unsigned int light;
            le_music_render_at(&scene, &f, &state, 1000U + t, out);
            report->travel += travel_q16(last_motion, state.motion_q16);
            last_motion = state.motion_q16;
            if (state.onset_env > prev_env)
                report->kicks++;
            prev_env = state.onset_env;
            light = total_light(out);
            if (light > report->peak_light)
                report->peak_light = light;
            if (light < report->low_light)
                report->low_light = light;
            if (lit_pixels(out) > report->max_lit)
                report->max_lit = lit_pixels(out);
            if (lit_pixels(out) < report->min_lit)
                report->min_lit = lit_pixels(out);
            if (!in_range(out))
                report->ranged = 0;
            for (k = 0U; k < distinct; k++)
                if (memcmp(seen[k], out, sizeof(out)) == 0)
                    break;
            if (k == distinct && distinct < 16U)
                memcpy(seen[distinct++], out, sizeof(out));
        }
        if (t % 500U == 499U) {
            report->windows++;
            if (distinct <= 1U)
                report->static_windows++;
            if (distinct < report->least_distinct)
                report->least_distinct = distinct;
            distinct = 0U;
        }
    }
}

/*
 * Feed a constant-feature frame stream (85 ms apart, as the producer sends at
 * ~11.7 fps) for three seconds.  Returns the longest run, in ms, for which the
 * rendered frame stayed identical, and the number of distinct brightest
 * pixels; reports the most pixels lit in any frame.
 */
static unsigned int longest_static_ms(enum le_music_grammar grammar,
                                      struct led_rgb accent,
                                      struct le_music_features base,
                                      int *distinct_peaks, int *max_lit)
{
    struct le_music_scene scene = scene_for(grammar, accent);
    struct le_music_render_state state;
    struct led_rgb out[LE_LED_PIXELS];
    struct led_rgb prev[LE_LED_PIXELS];
    int seen[LE_LED_PIXELS];
    unsigned int run = 0U, longest = 0U, t;
    int i;

    memset(seen, 0, sizeof(seen));
    *max_lit = 0;
    le_music_render_state_reset(&state, 1U);
    for (t = 0U; t <= 3000U; t += 85U) {
        struct le_music_features f = base;
        f.seq = t / 85U + 1U;
        f.timestamp_ms = 1000U + t;
        le_music_render(&scene, &f, &state, out);
        if (lit_pixels(out) > *max_lit)
            *max_lit = lit_pixels(out);
        seen[brightest(out)] = 1;
        if (t > 0U && memcmp(out, prev, sizeof(out)) == 0) {
            run += 85U;
            if (run > longest)
                longest = run;
        } else {
            run = 0U;
        }
        memcpy(prev, out, sizeof(out));
    }
    *distinct_peaks = 0;
    for (i = 0; i < LE_LED_PIXELS; i++)
        *distinct_peaks += seen[i];
    return longest;
}

/*
 * Simulate ledd timing for `ms` milliseconds: producer frames every
 * period_x10/10 ms (constant features, beat phase following bpm) and a 33 ms
 * render tick between them, each render on the frame timeline extended by
 * the time since the frame.  Returns the last rendered frame in out.
 */
static void run_stream(enum le_music_grammar grammar, struct led_rgb accent,
                       struct le_music_features base, unsigned int period_x10,
                       unsigned int ms, struct le_music_render_state *state,
                       struct led_rgb out[LE_LED_PIXELS])
{
    struct le_music_scene scene = scene_for(grammar, accent);
    struct le_music_features f = base;
    unsigned int t, next_x10 = 0U, seq = 0U;

    le_music_render_state_reset(state, 1U);
    for (t = 0U; t <= ms; t++) {
        int fresh = 0;
        if (t * 10U >= next_x10) {
            f = base;
            f.seq = ++seq;
            f.timestamp_ms = 1000U + t;
            if (base.bpm_x100 != 0U)
                f.beat_phase = (unsigned int)
                    (((unsigned long)t * base.bpm_x100 * 65536UL / 6000000UL) &
                     0xFFFFUL);
            next_x10 += period_x10;
            fresh = 1;
        }
        if (fresh || t % 33U == 0U)
            le_music_render_at(&scene, &f, state, 1000U + t, out);
    }
}

int main(void)
{
    struct led_rgb accent;
    struct le_music_render_state state;
    struct le_music_features f;
    struct led_rgb out[LE_LED_PIXELS];
    unsigned long sig[LE_GRAMMAR_COUNT];
    enum le_music_grammar g;

    require_condition(led_output_accent(LE_ACCENT_WARM_WHITE, &accent) == 1,
                      "calibrated accent must exist");

    /* 1. Every grammar has a distinct geometry signature. */
    for (g = LE_GRAMMAR_RIBBON; g < LE_GRAMMAR_COUNT; g++) {
        struct le_music_scene scene = scene_for(g, accent);
        int j;
        f = feat();
        le_music_render_state_reset(&state, 1U);
        le_music_render(&scene, &f, &state, out);
        sig[g] = signature(out);
        for (j = 0; j < (int)g; j++)
            require_condition(sig[j] != sig[g],
                              "grammars must render distinct geometry");
    }

    /* 2. Determinism: identical inputs render identical frames. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_ORBIT, accent);
        struct led_rgb out2[LE_LED_PIXELS];
        struct le_music_render_state state2;
        f = feat();
        le_music_render_state_reset(&state, 3U);
        le_music_render_state_reset(&state2, 3U);
        le_music_render(&scene, &f, &state, out);
        le_music_render(&scene, &f, &state2, out2);
        require_condition(memcmp(out, out2, sizeof(out)) == 0,
                          "the same scene must render identically");
    }

    /* 3. Sparse material keeps visible negative space for ribbon/ember. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_RIBBON, accent);
        f = feat();
        f.density = 10U;
        f.loudness_fast = 20U;
        f.energy = 30U;
        le_music_render_state_reset(&state, 1U);
        le_music_render(&scene, &f, &state, out);
        require_condition(lit_pixels(out) < LE_LED_PIXELS,
                          "sparse ribbon must leave dark pixels");
    }

    /* 4. Effects expose at most two near-white pixels, from the calibrated
       accent rather than raw white. */
    require_condition(!(accent.r == 255U && accent.g == 255U &&
                        accent.b == 255U),
                      "accent must not be raw 255,255,255");
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_RIBBON, accent);
        f = feat();

        scene.effect = LE_EFFECT_INHALE_CUT_BLOOM;
        scene.effect_center = 4U;
        scene.effect_age_ms = 800U;      /* 100..300 ms white window */
        le_music_render_state_reset(&state, 1U);
        le_music_render(&scene, &f, &state, out);
        require_condition(exact_matches(out, accent) == 1,
                          "inhale must show exactly one near-white lead pixel");

        scene.effect_age_ms = 600U;      /* dark cut */
        le_music_render(&scene, &f, &state, out);
        require_condition(lit_pixels(out) == 0, "inhale cut must be dark");

        scene.effect = LE_EFFECT_WHITE_APERTURE;
        scene.effect_age_ms = 100U;
        le_music_render(&scene, &f, &state, out);
        require_condition(exact_matches(out, accent) == 2,
                          "aperture must show exactly two near-white pixels");

        scene.effect = LE_EFFECT_KNIFE_SWEEP;
        scene.effect_age_ms = 120U;
        le_music_render(&scene, &f, &state, out);
        require_condition(exact_matches(out, accent) <= 2,
                          "knife sweep must stay within the accent budget");
        require_condition(exact_matches(out, accent) >= 1,
                          "knife sweep must show a moving point");

        scene.effect = LE_EFFECT_SHOCK_RING;
        scene.effect_age_ms = 60U;
        le_music_render(&scene, &f, &state, out);
        require_condition(exact_matches(out, accent) <= 2,
                          "shock ring must keep at most two white pixels");

        /* After the high-contrast window the accent must be gone. */
        scene.effect = LE_EFFECT_INHALE_CUT_BLOOM;
        scene.effect_age_ms = 1200U;     /* bloom phase */
        le_music_render(&scene, &f, &state, out);
        require_condition(exact_matches(out, accent) == 0,
                          "the near-white lead must not persist past its window");
    }

    /* 5. A crossfade blends two palettes without going dark. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_RIBBON, accent);
        struct le_music_scene faded = scene;
        f = feat();
        faded.next_palette.primary = (struct led_rgb){255U, 120U, 30U};
        faded.next_palette.secondary = (struct led_rgb){255U, 70U, 90U};
        faded.next_palette.accent = accent;
        faded.crossfade = 128U;
        le_music_render_state_reset(&state, 1U);
        le_music_render(&faded, &f, &state, out);
        require_condition(lit_pixels(out) > 0,
                          "mid-crossfade must still light the active scene");
    }

    /* 6. Constant music with low beat confidence must keep moving: no
       grammar may hold an identical frame for a second (hardware showed
       twin_arcs frozen for 9.9 s) and twin_arcs must never fill the ring. */
    {
        static const enum le_music_grammar moving[3] = {
            LE_GRAMMAR_TWIN_ARCS, LE_GRAMMAR_EMBER, LE_GRAMMAR_RIBBON
        };
        int k;
        for (k = 0; k < 3; k++) {
            int peaks, max_lit;
            unsigned int still;
            f = feat();
            f.beat_confidence = 40U;      /* typical: beat not locked */
            f.beat_phase = 12345U;        /* phase does not advance */
            f.density = 255U;             /* worst case for arc length */
            still = longest_static_ms(moving[k], accent, f, &peaks,
                                      &max_lit);
            if (still >= 1000U || peaks < 2)
                fprintf(stderr, "render: %s static %u ms, %d peak pixels\n",
                        le_music_grammar_name(moving[k]), still, peaks);
            require_condition(still < 1000U,
                              "constant music must not freeze the ring");
            require_condition(peaks >= 2,
                              "the bright point must travel around the ring");
            if (moving[k] == LE_GRAMMAR_TWIN_ARCS)
                require_condition(max_lit <= 8,
                                  "twin arcs must leave the ring partly dark");
            /* The chase is three dots a third of the ring apart: at rest
               each lights at most three pixels and the gaps stay black. */
            if (moving[k] == LE_GRAMMAR_EMBER)
                require_condition(max_lit <= 9,
                                  "ember must keep black space between its dots");
        }
    }
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_EMBER, accent);
        unsigned int t;
        le_music_render_state_reset(&state, 1U);
        for (t = 0U; t <= 3000U; t += 33U) {
            f = feat();
            f.beat_confidence = 40U;
            f.seq = t / 33U + 1U;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
            require_condition(lit_groups(out) == 3,
                              "the resting chase must be three separate dots");
        }
    }

    /* 7. A trusted tempo turns the ring three pixels per beat, with the
       tempo folded into one octave so half- and double-time estimates turn
       it at the same rate; below the trust level it free-runs.  Not a whole
       revolution per beat (which aliases at the producer frame rate). */
    {
        static const unsigned int tempos[4] = {12000U, 6000U, 24000U, 3000U};
        struct le_music_scene scene = scene_for(LE_GRAMMAR_RIBBON, accent);
        unsigned int k, t, moved;

        for (k = 0U; k < 5U; k++) {
            /* No hits and no beat wrap: the base rotation alone. */
            le_music_render_state_reset(&state, 1U);
            moved = 0U;
            for (t = 0U; t <= 2000U; t += 33U) {
                unsigned int before = state.motion_q16;
                f = feat();
                f.beat_confidence = k < 4U ? 220U : 63U;
                f.bpm_x100 = k < 4U ? tempos[k] : 12000U;
                f.seq = t / 33U + 1U;
                f.timestamp_ms = 1000U + t;
                le_music_render(&scene, &f, &state, out);
                moved += travel_q16(before, state.motion_q16);
            }
            if (k < 4U) {
                /* Four beats at 120 bpm: 12 px, one revolution, in 1.98 s. */
                if (moved < 770000U || moved > 790000U)
                    fprintf(stderr, "render: bpm %u moved %u\n", tempos[k],
                            moved);
                require_condition(moved >= 770000U && moved <= 790000U,
                                  "a trusted tempo must turn 3 px per beat");
            } else {
                /* 4.5 px/s plus up to 1.5 px/s with loudness. */
                require_condition(moved >= 9U * 65536U - 20000U &&
                                  moved <= 12U * 65536U,
                                  "an untrusted tempo must free-run");
            }
        }
    }
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_RIBBON, accent);
        int seen[LE_LED_PIXELS];
        int distinct = 0, i, last = -1, max_step = 0;
        unsigned int t, moved = 0U, kicks = 0U, prev_env = 0U;
        memset(seen, 0, sizeof(seen));
        le_music_render_state_reset(&state, 1U);
        for (t = 0U; t <= 2000U; t += 85U) {   /* four beats at 120 bpm */
            unsigned int before = state.motion_q16;
            f = feat();
            f.beat_confidence = 220U;
            f.bpm_x100 = 12000U;
            f.beat_phase = (t % 500U) * 65536U / 500U;
            f.seq = t / 85U + 1U;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
            moved += travel_q16(before, state.motion_q16);
            if (state.onset_env > prev_env)
                kicks++;
            prev_env = state.onset_env;
            if (last >= 0) {
                int step = (int)le_distance_px(last, brightest(out));
                if (step > max_step)
                    max_step = step;
            }
            last = brightest(out);
            seen[last] = 1;
        }
        for (i = 0; i < LE_LED_PIXELS; i++)
            distinct += seen[i];
        if (max_step > 1 || distinct != LE_LED_PIXELS || kicks < 3U)
            fprintf(stderr, "render: locked ribbon step %d, %d pixels, %u kicks, "
                    "moved %u\n", max_step, distinct, kicks, moved);
        require_condition(max_step <= 1,
                          "locked ribbon must not jump pixels between frames");
        /* The beat itself pulses and shoves the ring even with no kick
           onset in the mix, so it turns further than the base rotation. */
        require_condition(kicks >= 3U && kicks <= 4U,
                          "a tracked beat must pulse once per beat");
        require_condition(moved > 800000U && moved < 3U * 790000U,
                          "a beat must shove the rotation forward");
        require_condition(distinct == LE_LED_PIXELS,
                          "the locked ribbon must sweep the whole ring");
    }

    /* 8. A bass onset flashes and then decays within a few hundred ms. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_TWIN_ARCS, accent);
        unsigned int baseline = 0U, spike, later, settled = 0U, t;
        unsigned int seq = 1U;
        le_music_render_state_reset(&state, 1U);
        for (t = 0U; t < 1000U; t += 33U) {
            f = feat();
            f.beat_confidence = 40U;
            f.onset_low = 40U;
            f.seq = seq++;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
            baseline = total_light(out);
        }
        f.onset_low = 255U;
        f.seq = seq++;
        f.timestamp_ms = 1000U + t;
        le_music_render(&scene, &f, &state, out);
        spike = total_light(out);
        require_condition(spike > baseline + baseline / 4U,
                          "a bass onset must flash the ring");
        f.onset_low = 40U;
        t += 99U;
        f.seq = seq++;
        f.timestamp_ms = 1000U + t;
        le_music_render(&scene, &f, &state, out);
        later = total_light(out);
        require_condition(later < spike,
                          "the onset flash must start to decay");
        for (t += 33U; t < 1000U + 500U; t += 33U) {
            f.seq = seq++;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
            settled = total_light(out);
        }
        require_condition(settled < baseline + baseline / 8U,
                          "the onset flash must release back to the base level");
    }

    /* 9. ledd re-renders the last frame every 33 ms: the extended render
       clock keeps motion and the onset release running between frames, and
       a clock behind the last one holds rather than jumping. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_TWIN_ARCS, accent);
        struct led_rgb first[LE_LED_PIXELS];
        unsigned int flash, decayed;
        le_music_render_state_reset(&state, 1U);
        f = feat();
        f.beat_confidence = 40U;
        f.onset_low = 40U;
        f.seq = 1U;
        f.timestamp_ms = 1000U;
        le_music_render_at(&scene, &f, &state, 1000U, out);
        f.onset_low = 255U;
        f.seq = 2U;
        f.timestamp_ms = 1085U;
        le_music_render_at(&scene, &f, &state, 1085U, first);
        flash = total_light(first);
        le_music_render_at(&scene, &f, &state, 1085U + 66U, out);
        decayed = total_light(out);
        require_condition(memcmp(first, out, sizeof(out)) != 0,
                          "re-rendering one frame must keep the ring moving");
        require_condition(decayed < flash,
                          "the onset must decay between producer frames");
        memcpy(first, out, sizeof(out));
        le_music_render_at(&scene, &f, &state, 1085U, out);
        require_condition(memcmp(first, out, sizeof(out)) == 0,
                          "a clock step backwards must hold the ring");
    }

    /* 10. Producer frame rate (11.7 vs 23.4 fps) must not change speed.
       Free-running motion lands within 0.15 px after 3 s.  Beat-locked
       motion is also shoved by each beat, which the renderer can only see
       when a frame arrives (up to one frame period later at the slower
       rate), so it is held to 0.5 px: about 2 % of the ~20 px travelled,
       where a per-frame speed would double it.  The low-confidence orbit is
       placed by time, not per frame. */
    {
        struct le_music_render_state slow, fast;
        struct led_rgb out_slow[LE_LED_PIXELS], out_fast[LE_LED_PIXELS];
        int k;
        for (k = 0; k < 2; k++) {
            unsigned int d;
            unsigned int limit = k == 0 ? 9830U : 32768U;
            f = feat();
            f.beat_confidence = k == 0 ? 40U : 220U;
            f.bpm_x100 = k == 0 ? 0U : 12000U;
            run_stream(LE_GRAMMAR_TWIN_ARCS, accent, f, 854U, 3000U, &slow,
                       out_slow);
            run_stream(LE_GRAMMAR_TWIN_ARCS, accent, f, 427U, 3000U, &fast,
                       out_fast);
            d = slow.motion_q16 > fast.motion_q16
                ? slow.motion_q16 - fast.motion_q16
                : fast.motion_q16 - slow.motion_q16;
            if (d > limit)
                fprintf(stderr, "render: motion %u vs %u\n", slow.motion_q16,
                        fast.motion_q16);
            require_condition(d <= limit,
                              "motion speed must not depend on frame rate");
            require_condition(slow.motion_q16 != 65536U,   /* session 1 start */
                              "the ring must have moved");
        }
        f = feat();
        f.beat_confidence = 40U;
        run_stream(LE_GRAMMAR_ORBIT, accent, f, 854U, 2970U, &slow, out_slow);
        run_stream(LE_GRAMMAR_ORBIT, accent, f, 427U, 2970U, &fast, out_fast);
        require_condition(memcmp(out_slow, out_fast, sizeof(out_slow)) == 0,
                          "the free-running orbit must be placed by time");
    }

    /* 11. A latched event (KICK held for 4 frames at 23.4 fps, 2 at 11.7 fps)
       triggers one flash, not one per frame. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_TWIN_ARCS, accent);
        unsigned int period_x10;
        for (period_x10 = 427U; period_x10 <= 854U; period_x10 += 427U) {
            unsigned int t, next_x10 = 0U, seq = 0U, frame = 0U;
            unsigned int hold = period_x10 == 427U ? 4U : 2U;
            unsigned int kicks = 0U, prev_env = 0U, peak = 0U;
            le_music_render_state_reset(&state, 1U);
            f = feat();
            for (t = 0U; t <= 2000U; t++) {
                int fresh = 0;
                if (t * 10U >= next_x10) {
                    f = feat();
                    f.beat_confidence = 40U;
                    f.onset_low = 40U;
                    f.events = 0U;
                    if (frame >= 12U && frame < 12U + hold) {
                        f.events = LE_MUSIC_EVENT_KICK;   /* latched */
                        f.onset_low = 200U;               /* held level */
                    }
                    f.seq = ++seq;
                    f.timestamp_ms = 1000U + t;
                    next_x10 += period_x10;
                    frame++;
                    fresh = 1;
                }
                if (fresh || t % 33U == 0U) {
                    le_music_render_at(&scene, &f, &state, 1000U + t, out);
                    if (state.onset_env > prev_env)
                        kicks++;
                    if (state.onset_env > peak)
                        peak = state.onset_env;
                    prev_env = state.onset_env;
                }
            }
            if (kicks != 1U)
                fprintf(stderr, "render: %u kicks at period %u.%u ms\n", kicks,
                        period_x10 / 10U, period_x10 % 10U);
            require_condition(kicks == 1U,
                              "a latched event must flash once, not per frame");
            require_condition(peak >= 200U, "the latched kick must flash");
        }
    }

    /* 12. Every grammar is alive on a steady beat: under ledd timing no
       0.5 s window shows a static ring, the ring turns, each beat pulses,
       the frame breathes with the hits, and every channel stays in range. */
    for (g = LE_GRAMMAR_RIBBON; g < LE_GRAMMAR_COUNT; g++) {
        struct motion_report r;
        run_motion(g, accent, TRACE_BEAT, 6000U, &r);
        if (r.static_windows != 0U || r.least_distinct < 8U ||
            r.kicks < 11U || r.kicks > 13U ||
            r.peak_light < r.low_light * 2U)
            fprintf(stderr, "render: %s beat: %u/%u static, least %u frames, "
                    "%u kicks, light %u..%u, travel %u\n",
                    le_music_grammar_name(g), r.static_windows, r.windows,
                    r.least_distinct, r.kicks, r.low_light, r.peak_light,
                    r.travel);
        require_condition(r.windows == 12U, "the trace must cover 6 s");
        require_condition(r.static_windows == 0U,
                          "a steady beat must never leave the ring static");
        require_condition(r.least_distinct >= 8U,
                          "a steady beat must keep changing the ring");
        require_condition(r.kicks >= 11U && r.kicks <= 13U,
                          "every beat must punch the kick envelope once");
        /* 12 beats at 3 px each, plus the shove of every kick. */
        require_condition(r.travel > 36U * 65536U,
                          "a steady beat must turn and shove the ring");
        require_condition(r.peak_light >= r.low_light * 2U,
                          "hits must swell the frame");
        require_condition(r.ranged, "every channel must stay within 0..255");
        /* A hard hit may flare across the ring for a moment; the black
           space has to come back between hits. */
        require_condition(r.min_lit <= LE_LED_PIXELS / 2,
                          "black space must return between hits");
    }

    /* 13. With no beat lock and no hits every grammar still moves on its
       own clock: no 0.5 s window is static. */
    for (g = LE_GRAMMAR_RIBBON; g < LE_GRAMMAR_COUNT; g++) {
        struct motion_report r;
        run_motion(g, accent, TRACE_FREE, 6000U, &r);
        if (r.static_windows != 0U || r.least_distinct < 4U || r.kicks != 0U)
            fprintf(stderr, "render: %s free: %u/%u static, least %u frames, "
                    "%u kicks\n", le_music_grammar_name(g), r.static_windows,
                    r.windows, r.least_distinct, r.kicks);
        require_condition(r.static_windows == 0U && r.least_distinct >= 4U,
                          "steady sound without a beat must still move");
        require_condition(r.kicks == 0U,
                          "steady sound must not invent hits");
        require_condition(r.max_lit <= 9,
                          "the resting look must keep black space");
        /* 4.5..6 px/s for six seconds. */
        require_condition(r.travel >= 26U * 65536U &&
                          r.travel <= 36U * 65536U,
                          "the free-running ring must turn at its own speed");
        require_condition(r.low_light > 0U,
                          "audible music must never black the ring out");
        require_condition(r.ranged, "every channel must stay within 0..255");
    }

    /* 14. Silence dims every look instead of leaving a bright idle ring:
       the body is gated by loudness, so the propeller and the bloom go
       dark and the other grammars keep only a faint moving trace. */
    for (g = LE_GRAMMAR_RIBBON; g < LE_GRAMMAR_COUNT; g++) {
        struct motion_report quiet, sound;
        run_motion(g, accent, TRACE_SILENCE, 3000U, &quiet);
        run_motion(g, accent, TRACE_FREE, 3000U, &sound);
        if (quiet.peak_light * 2U > sound.low_light)
            fprintf(stderr, "render: %s silence %u vs sound %u\n",
                    le_music_grammar_name(g), quiet.peak_light,
                    sound.low_light);
        require_condition(quiet.peak_light * 2U <= sound.low_light,
                          "silence must dim the ring well below music");
        require_condition(quiet.kicks == 0U, "silence must not flash");
        if (g == LE_GRAMMAR_TWIN_ARCS || g == LE_GRAMMAR_SPLIT_PULSE)
            require_condition(quiet.peak_light == 0U,
                              "silence must leave this grammar dark");
    }

    /* 15. A kick reads in every grammar: more light than the resting frame,
       released again within half a second. */
    for (g = LE_GRAMMAR_RIBBON; g < LE_GRAMMAR_COUNT; g++) {
        struct le_music_scene scene = scene_for(g, accent);
        unsigned int rest = 0U, hit, after = 0U, t, seq = 1U;
        le_music_render_state_reset(&state, 1U);
        for (t = 0U; t < 1000U; t += 33U) {
            f = feat();
            f.beat_confidence = 40U;
            f.onset_low = f.onset_mid = f.onset_high = 0U;
            f.seq = seq++;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
            if (total_light(out) > rest)
                rest = total_light(out);       /* brightest resting frame */
        }
        f.onset_low = 255U;
        f.seq = seq++;
        f.timestamp_ms = 1000U + t;
        le_music_render(&scene, &f, &state, out);
        hit = total_light(out);
        if (hit <= rest + rest / 4U)
            fprintf(stderr, "render: %s rest %u hit %u\n",
                    le_music_grammar_name(g), rest, hit);
        require_condition(state.onset_env == 255U,
                          "a full kick must open the envelope fully");
        require_condition(hit > rest + rest / 4U,
                          "a kick must flash every grammar");
        f.onset_low = 0U;
        for (t += 33U; t < 1000U + 600U; t += 33U) {
            f.seq = seq++;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
            after = total_light(out);
        }
        require_condition(state.onset_env == 0U && after <= rest,
                          "the kick must release back to the resting look");
    }

    /* 16. A snare throws the chase into reverse, at most once per 450 ms,
       while the shared rotation keeps going forward. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_EMBER, accent);
        unsigned int t, seq = 1U, chase, forward;
        le_music_render_state_reset(&state, 1U);
        for (t = 0U; t < 600U; t += 33U) {
            f = feat();
            f.beat_confidence = 40U;
            f.onset_low = f.onset_mid = f.onset_high = 0U;
            f.seq = seq++;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
        }
        require_condition(!state.chase_reverse &&
                          state.chase_q16 == state.motion_q16,
                          "the chase must start forward with the ring");
        f.onset_mid = 220U;                     /* snare */
        f.seq = seq++;
        f.timestamp_ms = 1000U + t;
        le_music_render(&scene, &f, &state, out);
        require_condition(state.chase_reverse && state.snare_env > 0U,
                          "a snare must reverse the chase");
        chase = state.chase_q16;
        forward = state.motion_q16;
        f.onset_mid = 0U;
        t += 33U;
        f.seq = seq++;
        f.timestamp_ms = 1000U + t;
        le_music_render(&scene, &f, &state, out);
        require_condition(travel_q16(state.chase_q16, chase) ==
                              travel_q16(forward, state.motion_q16) &&
                          travel_q16(forward, state.motion_q16) > 0U,
                          "the reversed chase must run backwards");
        f.onset_mid = 220U;                     /* a second snare 66 ms on */
        t += 33U;
        f.seq = seq++;
        f.timestamp_ms = 1000U + t;
        le_music_render(&scene, &f, &state, out);
        require_condition(state.chase_reverse,
                          "a rapid second snare must not flip the chase back");
        f.onset_mid = 0U;
        for (t += 33U; t < 1300U; t += 33U) {
            f.seq = seq++;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
        }
        f.onset_mid = 220U;
        f.seq = seq++;
        f.timestamp_ms = 1000U + t;
        le_music_render(&scene, &f, &state, out);
        require_condition(!state.chase_reverse,
                          "a later snare must turn the chase forward again");
    }

    /* 17. Each kick opens the bloom five pixels on from the last one. */
    {
        struct le_music_scene scene = scene_for(LE_GRAMMAR_SPLIT_PULSE, accent);
        unsigned int t, seq = 1U, origin, hits = 0U;
        le_music_render_state_reset(&state, 1U);
        origin = state.bloom_origin;
        for (t = 0U; t < 2000U; t += 33U) {
            f = feat();
            f.beat_confidence = 40U;
            f.onset_mid = f.onset_high = 0U;
            f.onset_low = (t % 500U) < 33U && t > 0U ? 240U : 0U;
            f.seq = seq++;
            f.timestamp_ms = 1000U + t;
            le_music_render(&scene, &f, &state, out);
            if (state.kick_ripple_ms == 0U) {
                hits++;
                origin = (origin + 5U) % LE_LED_PIXELS;
                require_condition(out[origin].r || out[origin].g ||
                                  out[origin].b,
                                  "the bloom must open at its origin");
            }
            require_condition(state.bloom_origin == origin,
                              "the bloom origin must hop five pixels per kick");
        }
        require_condition(hits == 3U, "each kick must open one bloom");
    }

    puts("music renderer: ok");
    return 0;
}
