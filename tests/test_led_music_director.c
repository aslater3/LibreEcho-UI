/*
 * Focused behavioural tests for the scene director and musical transitions
 * (#64, #65).
 *
 * Build:
 *   gcc -std=c99 -Wall -Wextra -Wpedantic -O2 -o build/test-led-music-director \
 *       tests/test_led_music_director.c src/adapter/led_music_director.c \
 *       src/adapter/led_output.c
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

static int grammar_in_free_set(enum le_music_grammar g)
{
    return g == LE_GRAMMAR_RIBBON || g == LE_GRAMMAR_TWIN_ARCS ||
           g == LE_GRAMMAR_EMBER;
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

    /* 1. Steady input holds the scene: no grammar change, no effect. */
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

    /* 5. Low beat confidence never leaves a phase-locked orbit. */
    {
        struct le_music_director low;
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
        }
        require_condition(grammar_in_free_set(low.grammar) || low.crossfading,
                          "low confidence must escape the orbit grammar");
        if (low.crossfading)
            require_condition(grammar_in_free_set(low.pending_grammar),
                              "fallback target must be a free-running grammar");
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

    puts("music scene director: ok");
    return 0;
}
