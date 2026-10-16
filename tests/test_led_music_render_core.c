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

    puts("music renderer: ok");
    return 0;
}
