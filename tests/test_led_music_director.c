/*
 * Focused behavioural tests for the scene director and musical transitions
 * (#64, #65).
 *
 * Build:
 *   gcc -std=c99 -Wall -Wextra -Wpedantic -O2 -o build/test-led-music-director \
 *       tests/test_led_music_director.c src/adapter/led_music_director.c \
 *       src/adapter/led_music_render.c src/adapter/led_output.c \
 *       src/adapter/music_visualizer_protocol.c
 */
#include "../src/adapter/led_music_director.h"
#include "../src/adapter/music_visualizer_protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require_condition(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "director: %s\n", message);
        exit(1);
    }
}

static struct le_music_features feat(void)
{
    struct le_music_features f;
    memset(&f, 0, sizeof(f));
    f.feature_version = 2U;
    f.session = 1U;
    f.seq = 1U;
    f.timestamp_ms = 0U;
    f.energy = 120U;
    f.warmth = 128U;
    f.loudness_fast = 120U;
    f.loudness_slow = 120U;
    f.beat_confidence = 200U;
    return f;
}

/* Producer frame period used by the mood traces (~11.7 fps). */
#define FRAME_S 0.085

/*
 * One frame of music with a chosen mood score.  The director's score is
 * (loudness_fast - 110) + 1.5 * (brightness_axis - warmth).
 */
static struct le_music_features mood_feat(unsigned int loudness,
                                          unsigned int bright,
                                          unsigned int warmth)
{
    struct le_music_features f = feat();
    f.loudness_fast = loudness;
    f.loudness_slow = loudness;
    f.energy = loudness;
    f.brightness_axis = bright;
    f.warmth = warmth;
    return f;
}

/* Music whose score is exactly `heat` (tilt neutral, 48 <= loudness). */
static struct le_music_features heat_feat(int heat)
{
    return mood_feat((unsigned int)(110 + heat), 100U, 100U);
}

/* Feed frame number *frame (time = *frame * FRAME_S) and return its time. */
static double step(struct le_music_director *d, struct le_music_features f,
                   int *frame)
{
    double now = (double)*frame * FRAME_S;
    f.seq = (unsigned int)(*frame + 1);
    f.timestamp_ms = (unsigned int)(now * 1000.0);
    le_music_director_update(d, &f, now);
    (*frame)++;
    return now;
}

/* No libm: the focused harness links without -lm. */
static double absd(double v)
{
    return v < 0.0 ? -v : v;
}

static int rgb_equal(struct led_rgb a, struct led_rgb b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

static int palette_equal(const struct le_music_palette *a,
                         const struct le_music_palette *b)
{
    return rgb_equal(a->primary, b->primary) &&
           rgb_equal(a->secondary, b->secondary) &&
           rgb_equal(a->accent, b->accent);
}

static int scene_equal(const struct le_music_scene *a,
                       const struct le_music_scene *b)
{
    return a->grammar == b->grammar && a->next_grammar == b->next_grammar &&
           palette_equal(&a->palette, &b->palette) &&
           palette_equal(&a->next_palette, &b->next_palette) &&
           a->crossfade == b->crossfade && a->effect == b->effect &&
           a->effect_age_ms == b->effect_age_ms &&
           a->effect_center == b->effect_center;
}

/* Red/orange family: red saturated and clearly above green and blue. */
static int is_hot(struct led_rgb c)
{
    return c.r >= 200U && c.r >= 2U * c.g && c.r >= 4U * c.b;
}

/* Blue/teal family: almost no red, carried by blue and green. */
static int is_cool(struct led_rgb c)
{
    unsigned int top = c.g > c.b ? c.g : c.b;
    return c.r < 64U && top >= 150U && c.g + c.b >= 4U * c.r;
}

/*
 * A deterministic trace that walks the moods: calm, loud and bright, silence,
 * a section event, then mid-level music.
 */
static struct le_music_features trace_feat(int frame)
{
    double at = (double)frame * FRAME_S;
    struct le_music_features f;

    if (at < 6.0)
        f = mood_feat(80U, 60U, 180U);
    else if (at < 15.0)
        f = mood_feat(230U, 200U, 60U);
    else if (at < 17.0)
        f = mood_feat(20U, 0U, 0U);
    else
        f = heat_feat(40);
    if (frame == 260) {
        f.events = LE_MUSIC_EVENT_SECTION;
        f.novelty = 210U;
    }
    return f;
}

int main(void)
{
    struct le_music_director config;
    struct le_music_director_config cfg;
    struct le_music_scene scene;
    struct le_music_features f;
    double t;
    int i;

    le_music_director_config_defaults(&cfg);
    require_condition(cfg.scene_dwell_seconds >= 10.0 - 0.001,
                      "default dwell must be at least ten seconds");
    require_condition(cfg.effect_cooldown_seconds >= 8.0 - 0.001,
                      "default major-effect cooldown must be at least 8s");
    require_condition(cfg.scene_crossfade_seconds >= 0.7 &&
                      cfg.scene_crossfade_seconds <= 1.5,
                      "default crossfade must be around one second");

    /* 1. Steady input holds the scene: no grammar change, no effect.  (Two
       seconds: the opening palette is replaced after three, see 16.) */
    le_music_director_reset(&config, 7U, &cfg);
    {
        enum le_music_grammar start = config.grammar;
        for (i = 0, t = 0.0; i < 40; i++, t += 0.05) {
            f = feat();
            f.seq = (unsigned int)(i + 1);
            f.timestamp_ms = (unsigned int)(t * 1000.0);
            le_music_director_update(&config, &f, t);
        }
        le_music_director_scene(&config, t, &scene);
        require_condition(config.grammar == start,
                          "steady input must not change the grammar");
        require_condition(scene.crossfade == 0U, "steady input must not crossfade");
        require_condition(config.effect == LE_EFFECT_NONE,
                          "ordinary steady beats must not trigger an effect");
    }

    /* 2. A single ordinary transient does not change the scene. */
    {
        enum le_music_grammar before = config.grammar;
        f = feat();
        f.seq = 100U;
        f.timestamp_ms = 5000U;
        f.events = LE_MUSIC_EVENT_SNARE | LE_MUSIC_EVENT_KICK;
        le_music_director_update(&config, &f, t + 0.05);
        require_condition(config.grammar == before,
                          "a single transient must not change the grammar");
    }

    /* 3. A section event after the dwell begins a crossfade that settles. */
    {
        double change_time = 12.0;
        enum le_music_grammar before = config.grammar;
        f = feat();
        f.seq = 200U;
        f.timestamp_ms = 12000U;
        f.events = LE_MUSIC_EVENT_SECTION;
        f.novelty = 200U;
        f.beat_confidence = 180U;
        le_music_director_update(&config, &f, change_time);
        le_music_director_scene(&config, change_time, &scene);
        require_condition(config.crossfading, "section must start a crossfade");
        require_condition(scene.crossfade < 128U,
                          "crossfade must start near the outgoing scene");
        require_condition(config.pending_grammar != before,
                          "a change must pick a different grammar");
        le_music_director_update(&config, &f, change_time + 1.2);
        require_condition(!config.crossfading,
                          "crossfade must settle after about one second");
        require_condition(config.grammar == config.pending_grammar,
                          "settled grammar must be the pending one");
    }

    /* 4. Determinism: identical trace, identical decisions and frames. */
    {
        struct le_music_director a, b;
        struct le_music_scene sa, sb;
        struct le_music_features fa, fb;
        double at = 0.0;
        le_music_director_reset(&a, 99U, &cfg);
        le_music_director_reset(&b, 99U, &cfg);
        for (i = 0; i < 60; i++, at += 0.07) {
            fa = feat(); fb = fa;
            fa.seq = fb.seq = (unsigned int)i;
            fa.timestamp_ms = fb.timestamp_ms = (unsigned int)(at * 1000.0);
            if (i == 30) {
                fa.events = fb.events = LE_MUSIC_EVENT_SECTION;
                fa.novelty = fb.novelty = 210U;
            }
            le_music_director_update(&a, &fa, at);
            le_music_director_update(&b, &fb, at);
        }
        le_music_director_scene(&a, at, &sa);
        le_music_director_scene(&b, at, &sb);
        require_condition(sa.grammar == sb.grammar &&
                          sa.next_grammar == sb.next_grammar &&
                          sa.crossfade == sb.crossfade,
                          "the same trace must decide identically");
    }

    /* 5. Every grammar runs on its own clock, so low beat confidence neither
       forces the director out of orbit nor narrows what it may pick next. */
    {
        struct le_music_director low;
        int seen[LE_GRAMMAR_COUNT];
        unsigned int session;
        int kinds = 0;

        le_music_director_reset(&low, 5U, &cfg);
        low.grammar = LE_GRAMMAR_ORBIT;
        low.pending_grammar = LE_GRAMMAR_ORBIT;
        for (i = 0, t = 0.0; i < 10; i++, t += 0.05) {
            f = feat();
            f.seq = (unsigned int)i;
            f.timestamp_ms = (unsigned int)(t * 1000.0);
            f.beat_confidence = 20U;
            f.energy = 60U;
            f.loudness_fast = 60U;
            le_music_director_update(&low, &f, t);
            require_condition(low.grammar == LE_GRAMMAR_ORBIT &&
                              !low.crossfading,
                              "low confidence must not force a change of grammar");
        }
        require_condition(low.confidence_ema < 90U,
                          "the trace must read as low confidence");

        memset(seen, 0, sizeof(seen));
        for (session = 1U; session <= 40U; session++) {
            int frame = 0;
            le_music_director_reset(&low, session, &cfg);
            while (!low.crossfading && frame < 60) {
                f = feat();
                f.beat_confidence = 20U;
                (void)step(&low, f, &frame);
            }
            require_condition(low.crossfading,
                              "the opening scene must be replaced");
            require_condition(low.pending_grammar != low.grammar,
                              "a change must pick a different grammar");
            seen[low.pending_grammar] = 1;
        }
        for (i = 0; i < (int)LE_GRAMMAR_COUNT; i++)
            kinds += seen[i];
        require_condition(kinds == (int)LE_GRAMMAR_COUNT,
                          "low confidence must leave every grammar available");
    }

    /* 6. Steady ordinary beats never fabricate a major transition. */
    {
        le_music_director_reset(&config, 11U, &cfg);
        for (i = 0, t = 0.0; i < 120; i++, t += 0.05) {
            f = feat();
            f.seq = (unsigned int)i;
            f.timestamp_ms = (unsigned int)(t * 1000.0);
            f.energy = 200U;
            f.loudness_fast = 200U;
            f.events = LE_MUSIC_EVENT_KICK;
            f.beat_confidence = 255U;
            le_music_director_update(&config, &f, t);
        }
        require_condition(config.effect == LE_EFFECT_NONE,
                          "a dense steady chorus must not trigger a transition");
    }

    /* 7. Build then drop triggers inhale/cut/bloom at the boundary. */
    {
        double now = 0.0;
        le_music_director_reset(&config, 21U, &cfg);
        for (i = 0; i < 30; i++, now += 0.05) {
            f = feat();
            f.seq = (unsigned int)i;
            f.timestamp_ms = (unsigned int)(now * 1000.0);
            f.events = LE_MUSIC_EVENT_BUILD;
            f.build = 200U;
            f.beat_confidence = 180U;
            le_music_director_update(&config, &f, now);
            require_condition(config.effect == LE_EFFECT_NONE,
                              "a build alone must not fire the drop effect");
        }
        f = feat();
        f.seq = 500U;
        f.timestamp_ms = (unsigned int)(now * 1000.0);
        f.events = LE_MUSIC_EVENT_DROP;
        f.event_strength = 200U;
        f.novelty = 190U;
        f.beat_confidence = 180U;
        le_music_director_update(&config, &f, now);
        require_condition(config.effect == LE_EFFECT_INHALE_CUT_BLOOM,
                          "a confident build/drop must trigger inhale/cut/bloom");
    }

    /* 8. Global cooldown blocks a second transition until it elapses. */
    {
        double base = config.last_effect;
        le_music_director_update(&config, &f, base + 2.0);   /* effect expired */
        require_condition(config.effect == LE_EFFECT_NONE,
                          "the transition must expire");
        f.seq = 501U;
        f.timestamp_ms += 2000U;
        f.events = LE_MUSIC_EVENT_DROP;
        le_music_director_update(&config, &f, base + 2.0);
        require_condition(config.effect == LE_EFFECT_NONE,
                          "cooldown must block a rapid second transition");
        f.build = 200U;                 /* the drop still carries build energy */
        le_music_director_update(&config, &f, base + 9.0);
        require_condition(config.effect == LE_EFFECT_INHALE_CUT_BLOOM,
                          "a transition after the cooldown must fire");
    }

    /* 9. Quiet gap then re-entry uses the structural evidence. */
    {
        double now = 0.0;
        le_music_director_reset(&config, 33U, &cfg);
        for (i = 0; i < 20; i++, now += 0.05) {
            f = feat();
            f.seq = (unsigned int)i;
            f.timestamp_ms = (unsigned int)(now * 1000.0);
            f.energy = 10U;
            f.loudness_fast = 10U;
            f.build = 0U;
            f.beat_confidence = 60U;
            le_music_director_update(&config, &f, now);
        }
        f = feat();
        f.seq = 900U;
        f.timestamp_ms = (unsigned int)(now * 1000.0);
        f.events = LE_MUSIC_EVENT_REENTRY;
        f.event_strength = 180U;
        f.novelty = 160U;
        f.beat_confidence = 150U;
        le_music_director_update(&config, &f, now);
        require_condition(config.effect == LE_EFFECT_INHALE_CUT_BLOOM,
                          "a quiet gap plus re-entry must trigger the transition");
    }

    /* 10. Interruption aborts without replaying and keeps the scene. */
    {
        enum le_music_grammar before = config.grammar;
        le_music_director_interrupt(&config);
        require_condition(config.effect == LE_EFFECT_NONE,
                          "interrupt must abort the transition");
        require_condition(config.grammar == before,
                          "interrupt must not change the base scene");
        f.seq = 901U;
        f.timestamp_ms += 100U;
        f.events = LE_MUSIC_EVENT_DROP;
        le_music_director_update(&config, &f, config.last_effect + 1.0);
        require_condition(config.effect == LE_EFFECT_NONE,
                          "an interrupted transition must not replay");
    }

    /* 11. Without structural events or a change of mood a settled scene is
       refreshed at 1.2 dwells: not before the dwell, and not held for two
       dwells (~21 s observed on hardware). */
    {
        struct le_music_director d;
        int frame = 0, changes = 0, was_fading = 0;
        double settled = -1.0, refreshed = -1.0;
        le_music_director_reset(&d, 17U, &cfg);
        while (frame * FRAME_S < 30.0) {
            f = feat();
            f.beat_confidence = 40U;
            t = step(&d, f, &frame);
            if (d.crossfading && !was_fading) {
                changes++;
                if (changes == 2)
                    refreshed = t;
            }
            if (!d.crossfading && was_fading && changes == 1)
                settled = t;
            was_fading = d.crossfading;
        }
        require_condition(settled > 0.0 && refreshed > 0.0,
                          "a steady segment must change within ~1.2 dwells");
        if (refreshed - settled < cfg.scene_dwell_seconds ||
            refreshed - settled > cfg.scene_dwell_seconds * 1.2 + 2.0 * FRAME_S)
            fprintf(stderr, "director: settled %.3f refreshed %.3f\n", settled,
                    refreshed);
        require_condition(refreshed - settled >= cfg.scene_dwell_seconds,
                          "a scene without structure must last the dwell");
        require_condition(refreshed - settled <=
                              cfg.scene_dwell_seconds * 1.2 + 2.0 * FRAME_S,
                          "a steady segment must change within ~1.2 dwells");
    }

    /* 12. Confidence tracking follows time, not frame count: a drop in
       beat confidence reads the same at 11.7 and 23.4 fps. */
    {
        struct le_music_director slow, fast;
        double step;
        unsigned int diff;
        for (step = 0.0854; step > 0.04; step /= 2.0) {
            struct le_music_director *d = step > 0.06 ? &slow : &fast;
            le_music_director_reset(d, 23U, &cfg);
            for (i = 0, t = 0.0; t < 1.3; i++, t += step) {
                f = feat();
                f.seq = (unsigned int)(i + 1);
                f.timestamp_ms = (unsigned int)(t * 1000.0);
                f.beat_confidence = t < 1.0 ? 200U : 40U;
                le_music_director_update(d, &f, t);
            }
        }
        diff = slow.confidence_ema > fast.confidence_ema
             ? slow.confidence_ema - fast.confidence_ema
             : fast.confidence_ema - slow.confidence_ema;
        if (diff > 12U)
            fprintf(stderr, "director: ema %u vs %u\n", slow.confidence_ema,
                    fast.confidence_ema);
        require_condition(diff <= 12U,
                          "confidence smoothing must not depend on frame rate");
    }

    /* ---- Mood palettes.  The mood score ("heat") averages loudness and
       spectral tilt over seconds and picks the palette family. ---------- */

    require_condition(strcmp(le_music_mood_name(LE_MOOD_CALM), "calm") == 0 &&
                      strcmp(le_music_mood_name(LE_MOOD_BALANCED),
                             "balanced") == 0 &&
                      strcmp(le_music_mood_name(LE_MOOD_ENERGETIC),
                             "energetic") == 0 &&
                      strcmp(le_music_mood_name(LE_MOOD_INTENSE),
                             "intense") == 0 &&
                      strcmp(le_music_mood_name(LE_MOOD_COUNT), "none") == 0,
                      "moods must have their published names");

    /* 13. Sustained loud, bright-tilted music reaches the intense mood and a
       red/orange palette; sustained quiet, warm music reaches calm and a
       blue/teal palette.  Both start from the same neutral opening. */
    {
        unsigned int session;
        for (session = 1U; session <= 8U; session++) {
            struct le_music_director hot, cool;
            int frame = 0;

            le_music_director_reset(&hot, session, &cfg);
            le_music_director_reset(&cool, session, &cfg);
            require_condition(hot.mood == LE_MOOD_BALANCED &&
                              palette_equal(&hot.palette, &cool.palette),
                              "a session must open on a balanced palette");
            while (frame * FRAME_S < 6.0) {
                int n = frame;
                (void)step(&hot, mood_feat(230U, 200U, 60U), &n);
                (void)step(&cool, mood_feat(70U, 40U, 200U), &frame);
                if (frame == 2)
                    /* The first seconds are a running mean, so the first
                       music heard sets the score outright. */
                    require_condition(absd(hot.heat - 330.0) < 1e-9 &&
                                      absd(cool.heat + 280.0) < 1e-9,
                                      "the mood score must start from the music");
            }
            require_condition(!hot.crossfading && !cool.crossfading,
                              "the mood palette must have settled");
            require_condition(hot.heat > 300.0 && cool.heat < -250.0,
                              "the mood score must follow the music");

            le_music_director_scene(&hot, 6.0, &scene);
            require_condition(hot.mood == LE_MOOD_INTENSE,
                              "loud bright music must reach the intense mood");
            if (!is_hot(scene.palette.primary))
                fprintf(stderr, "director: session %u primary %u,%u,%u\n",
                        session, scene.palette.primary.r,
                        scene.palette.primary.g, scene.palette.primary.b);
            require_condition(is_hot(scene.palette.primary) &&
                              scene.palette.secondary.r >
                                  scene.palette.secondary.g &&
                              scene.palette.secondary.r >
                                  scene.palette.secondary.b,
                              "the intense palette must be red/orange");

            le_music_director_scene(&cool, 6.0, &scene);
            require_condition(cool.mood == LE_MOOD_CALM,
                              "quiet warm music must reach the calm mood");
            if (!is_cool(scene.palette.primary))
                fprintf(stderr, "director: session %u primary %u,%u,%u\n",
                        session, scene.palette.primary.r,
                        scene.palette.primary.g, scene.palette.primary.b);
            require_condition(is_cool(scene.palette.primary) &&
                              scene.palette.secondary.b >
                                  scene.palette.secondary.r &&
                              scene.palette.secondary.g >
                                  scene.palette.secondary.r,
                              "the calm palette must be blue/teal");
        }
    }

    /* 14. Hysteresis: a score hovering within +-5 of the balanced/energetic
       edge (45) keeps whichever mood it came from. */
    {
        struct le_music_director d;
        enum le_music_mood last;
        int frame = 0, shifts = 0, phase, n;
        double low = 1000.0, high = -1000.0;

        le_music_director_reset(&d, 3U, &cfg);
        last = d.mood;
        /* Phases of 20 s: hover x6, push above the edge, hover x6, drop. */
        for (phase = 0; phase < 14; phase++) {
            int heat = phase == 6 ? 60 : phase == 13 ? 30
                     : (phase % 2 == 0 ? 50 : 40);
            for (n = 0; n < 235; n++) {
                (void)step(&d, heat_feat(heat), &frame);
                if (d.mood != last) {
                    shifts++;
                    last = d.mood;
                }
                if (phase < 6)
                    require_condition(d.mood == LE_MOOD_BALANCED,
                                      "hovering below must stay balanced");
                if (phase >= 8 && phase < 13)
                    require_condition(d.mood == LE_MOOD_ENERGETIC,
                                      "hovering after a rise must stay energetic");
                if ((phase >= 2 && phase < 6) || (phase >= 9 && phase < 13)) {
                    if (d.heat < low)
                        low = d.heat;
                    if (d.heat > high)
                        high = d.heat;
                }
            }
            if (phase == 6)
                require_condition(d.mood == LE_MOOD_ENERGETIC,
                                  "passing the edge by the margin must shift up");
        }
        if (low > 42.0 || low < 39.9 || high < 48.0 || high > 50.1)
            fprintf(stderr, "director: hover range %.2f..%.2f\n", low, high);
        require_condition(low <= 42.0 && low >= 39.9 && high >= 48.0 &&
                          high <= 50.1,
                          "the hover must cross the edge both ways");
        require_condition(d.mood == LE_MOOD_BALANCED,
                          "passing the edge by the margin must shift down");
        require_condition(shifts == 2,
                          "a hovering score must not flip the mood");
    }

    /* 15. Silence (loudness_fast < 48) says nothing about the mood: it moves
       neither the score nor the opening palette's clock. */
    {
        struct le_music_director d;
        struct le_music_palette opening;
        struct le_music_features quiet = mood_feat(47U, 255U, 0U);
        int frame = 0;
        double heat, heard;

        le_music_director_reset(&d, 9U, &cfg);
        opening = d.palette;
        while (frame * FRAME_S < 2.0)
            (void)step(&d, heat_feat(30), &frame);
        heat = d.heat;
        heard = d.heat_seconds;
        require_condition(absd(heat - 30.0) < 1e-6 && heard > 1.8,
                          "the music before the silence must be heard");
        while (frame * FRAME_S < 7.0) {
            (void)step(&d, quiet, &frame);
            require_condition(d.heat == heat && d.heat_seconds == heard,
                              "silence must not move the mood score");
            require_condition(!d.crossfading &&
                              palette_equal(&d.palette, &opening),
                              "silence must not replace the opening palette");
        }
        quiet.loudness_fast = 48U;      /* the same frame, just audible */
        (void)step(&d, quiet, &frame);
        require_condition(d.heat > heat + 1.0 && d.heat_seconds > heard,
                          "audible music must move the mood score");
    }

    /* 16. The opening palette predates the music: it is replaced once three
       seconds of music have been heard, not three seconds of silence.  The
       music here stays in the opening (balanced) mood, so only this rule can
       recolour the ring. */
    {
        int lead;
        for (lead = 0; lead <= 2; lead += 2) {
            struct le_music_director d;
            struct le_music_palette opening;
            int frame = 0;
            double started = -1.0;

            le_music_director_reset(&d, 12U, &cfg);
            opening = d.palette;
            while (frame * FRAME_S < 8.0) {
                t = (double)frame * FRAME_S;
                (void)step(&d, t < (double)lead ? mood_feat(10U, 0U, 0U)
                                               : heat_feat(30),
                           &frame);
                if (d.crossfading && started < 0.0) {
                    started = t;
                    require_condition(!palette_equal(&d.pending_palette,
                                                     &opening) &&
                                      !rgb_equal(d.pending_palette.primary,
                                                 opening.primary),
                                      "the opening must give way to a new palette");
                }
                if (started < 0.0)
                    require_condition(palette_equal(&d.palette, &opening),
                                      "the opening palette must hold until then");
            }
            if (started < (double)lead + 3.0 - FRAME_S ||
                started > (double)lead + 3.0 + 2.0 * FRAME_S)
                fprintf(stderr, "director: lead %d opening replaced at %.3f\n",
                        lead, started);
            require_condition(started >= (double)lead + 3.0 - FRAME_S &&
                              started <= (double)lead + 3.0 + 2.0 * FRAME_S,
                              "the opening must be replaced after ~3 s of music");
            require_condition(!d.crossfading &&
                              !palette_equal(&d.palette, &opening) &&
                              d.mood == LE_MOOD_BALANCED,
                              "the replacement palette must settle");
        }
    }

    /* 17. A change of mood recolours the ring without waiting for the dwell,
       but only once the scene has shown for four seconds, and always with a
       palette other than the one showing. */
    {
        struct le_music_director d;
        struct le_music_palette showing;
        int frame = 0;
        double crossed = -1.0, age = 0.0;

        le_music_director_reset(&d, 4U, &cfg);
        while (frame * FRAME_S < 20.0 &&
               (d.crossfading || d.scene_started == d.first_update))
            (void)step(&d, heat_feat(-20), &frame);
        require_condition(!d.crossfading && d.mood == LE_MOOD_CALM,
                          "calm music must settle on a calm scene");
        showing = d.palette;
        require_condition(is_cool(showing.primary),
                          "the calm scene must show a calm palette");

        while (!d.crossfading && frame * FRAME_S < 20.0) {
            t = step(&d, mood_feat(230U, 200U, 60U), &frame);
            age = t - d.scene_started;
            if (crossed < 0.0 && d.heat >= 26.0)
                crossed = age;
            if (age < 4.0)
                require_condition(!d.crossfading,
                                  "a mood shift must wait for 4 s of scene time");
        }
        if (crossed < 0.0 || crossed > 3.0 || age > 4.0 + 2.0 * FRAME_S)
            fprintf(stderr, "director: mood crossed at %.3f, recoloured at %.3f\n",
                    crossed, age);
        require_condition(crossed >= 0.0 && crossed <= 3.0,
                          "the mood must have changed well inside the scene");
        require_condition(d.crossfading && age >= 4.0 &&
                          age <= 4.0 + 2.0 * FRAME_S,
                          "a mood shift must recolour at 4 s, before the dwell");
        require_condition(d.mood > LE_MOOD_CALM,
                          "the recolour must follow the new mood");
        require_condition(!palette_equal(&d.pending_palette, &showing) &&
                          !rgb_equal(d.pending_palette.primary,
                                     showing.primary) &&
                          !is_cool(d.pending_palette.primary),
                          "a mood shift must bring a different palette");
        le_music_director_scene(&d, t, &scene);
        require_condition(palette_equal(&scene.palette, &showing) &&
                          palette_equal(&scene.next_palette,
                                        &d.pending_palette),
                          "the scene must crossfade from the old palette");
    }

    /* 18. Within one mood every refresh still changes the palette: the one
       showing is never picked again. */
    {
        unsigned int session;
        int changes = 0;
        for (session = 1U; session <= 20U; session++) {
            struct le_music_director d;
            struct le_music_palette showing;
            int frame = 0;

            le_music_director_reset(&d, session, &cfg);
            showing = d.palette;
            while (frame * FRAME_S < 60.0) {
                int was_fading = d.crossfading;
                (void)step(&d, heat_feat(-20), &frame);
                if (was_fading && !d.crossfading) {
                    require_condition(!palette_equal(&d.palette, &showing),
                                      "a refresh must not repeat the palette");
                    require_condition(d.mood == LE_MOOD_CALM &&
                                      is_cool(d.palette.primary),
                                      "a calm refresh must stay in the family");
                    showing = d.palette;
                    changes++;
                }
            }
        }
        require_condition(changes >= 80,
                          "steady music must keep refreshing the palette");
    }

    /* 19. Determinism: the same trace and session give the same scene on
       every frame, through mood shifts, silence and a section event; the
       session seeds the variation. */
    {
        struct le_music_director a, b;
        struct le_music_scene sa, sb;
        unsigned int session;
        int moods[LE_MOOD_COUNT];
        int frame, varied = 0;

        memset(moods, 0, sizeof(moods));
        le_music_director_reset(&a, 99U, &cfg);
        le_music_director_reset(&b, 99U, &cfg);
        for (frame = 0; frame < 420; ) {
            int n = frame;
            (void)step(&a, trace_feat(frame), &n);
            t = step(&b, trace_feat(frame), &frame);
            le_music_director_scene(&a, t, &sa);
            le_music_director_scene(&b, t, &sb);
            require_condition(scene_equal(&sa, &sb) && a.mood == b.mood &&
                              a.heat == b.heat,
                              "the same trace and session must give the same scene");
            moods[a.mood] = 1;
        }
        require_condition(moods[LE_MOOD_CALM] && moods[LE_MOOD_INTENSE],
                          "the trace must walk through the moods");
        for (session = 100U; session < 106U && !varied; session++) {
            le_music_director_reset(&b, session, &cfg);
            for (frame = 0; frame < 420; )
                t = step(&b, trace_feat(frame), &frame);
            le_music_director_scene(&b, t, &sb);
            if (!scene_equal(&sa, &sb))
                varied = 1;
        }
        require_condition(varied, "the session must seed the variation");
    }

    puts("music scene director: ok");
    return 0;
}
