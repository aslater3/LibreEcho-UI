#include "led_music_render.h"
#include "music_visualizer_protocol.h"

#include <string.h>

static unsigned int le_mul255(unsigned int value, unsigned int factor)
{
    return (value * factor + 127U) / 255U;
}

static struct led_rgb le_lerp(struct led_rgb a, struct led_rgb b,
                              unsigned int t)
{
    struct led_rgb out;

    out.r = (a.r * (255U - t) + b.r * t + 127U) / 255U;
    out.g = (a.g * (255U - t) + b.g * t + 127U) / 255U;
    out.b = (a.b * (255U - t) + b.b * t + 127U) / 255U;
    return out;
}

static void le_add(struct led_rgb *pixel, struct led_rgb colour,
                   unsigned int factor)
{
    unsigned int r = le_mul255(colour.r, factor);
    unsigned int g = le_mul255(colour.g, factor);
    unsigned int b = le_mul255(colour.b, factor);

    pixel->r = pixel->r + r > 255U ? 255U : pixel->r + r;
    pixel->g = pixel->g + g > 255U ? 255U : pixel->g + g;
    pixel->b = pixel->b + b > 255U ? 255U : pixel->b + b;
}

/* Replace a pixel with a scaled colour: used for calibrated white leads so
   they read as the named near-white point rather than base + white. */
static void le_set(struct led_rgb *pixel, struct led_rgb colour,
                   unsigned int factor)
{
    pixel->r = le_mul255(colour.r, factor);
    pixel->g = le_mul255(colour.g, factor);
    pixel->b = le_mul255(colour.b, factor);
}

static unsigned int le_distance(unsigned int a, unsigned int b)
{
    unsigned int d = a > b ? a - b : b - a;
    return d > LE_LED_PIXELS / 2U ? LE_LED_PIXELS - d : d;
}

void le_music_render_state_reset(struct le_music_render_state *state,
                                 unsigned int session)
{
    if (state == NULL)
        return;
    memset(state, 0, sizeof(*state));
    state->free_phase = session % LE_LED_PIXELS;
}

/* ------------------------------- grammars ------------------------------- */

static void le_render_ribbon(struct led_rgb out[LE_LED_PIXELS],
                             const struct le_music_features *f,
                             struct led_rgb primary, struct led_rgb secondary,
                             unsigned int warmth)
{
    unsigned int width = 2U + f->loudness_fast / 70U;   /* 2..5 */
    unsigned int center = (f->beat_phase * LE_LED_PIXELS) / 65536U;
    unsigned int level = 30U + f->energy / 2U;          /* 30..157 */
    struct led_rgb ribbon = le_lerp(primary, secondary, warmth);
    unsigned int i;

    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int d = le_distance(i, center % LE_LED_PIXELS);
        if (d <= width) {
            unsigned int frac = (width - d) * 255U / width;
            le_add(&out[i], ribbon, le_mul255(frac, level));
        }
    }
}

static void le_render_twin_arcs(struct led_rgb out[LE_LED_PIXELS],
                                const struct le_music_features *f,
                                struct led_rgb primary,
                                struct led_rgb secondary)
{
    unsigned int length = 2U + f->density / 64U;        /* 2..5 */
    unsigned int weight = 40U + f->loudness_slow / 2U;  /* 40..167 */
    unsigned int i;

    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int d0 = le_distance(i, 0U);
        unsigned int d1 = le_distance(i, 6U);
        unsigned int d = d0 < d1 ? d0 : d1;
        if (d < length) {
            unsigned int frac = (length - d) * 255U / length;
            struct led_rgb colour = (d0 <= d1) ? primary : secondary;
            le_add(&out[i], colour, le_mul255(frac, weight));
        }
    }
    /* Sparse transient accents rather than filling the ring. */
    if (f->onset_high > 120U) {
        unsigned int accent = f->onset_high;
        le_add(&out[3U], primary, accent);
        le_add(&out[9U], secondary, accent);
    }
}

static void le_render_orbit(struct led_rgb out[LE_LED_PIXELS],
                            const struct le_music_features *f,
                            struct le_music_render_state *state,
                            struct led_rgb primary, struct led_rgb secondary)
{
    unsigned int center;
    unsigned int size = 1U + f->onset_low / 90U;
    unsigned int level = 40U + f->energy / 2U;
    unsigned int i;

    if (f->beat_confidence >= 110U) {
        center = (f->beat_phase * LE_LED_PIXELS) / 65536U;
    } else {
        /* Free-running and deterministic: position advances with the
           producer's monotonic timestamp, never with beat phase. */
        state->free_phase = (f->timestamp_ms / 180U + state->free_phase) %
                            LE_LED_PIXELS;
        center = state->free_phase;
    }
    if (f->events & LE_MUSIC_EVENT_SNARE)
        size++;
    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int d0 = le_distance(i, center);
        unsigned int d1 = le_distance(i, (center + 6U) % LE_LED_PIXELS);
        unsigned int d = d0 < d1 ? d0 : d1;
        if (d < size) {
            unsigned int frac = (size - d) * 255U / size;
            struct led_rgb colour = (d0 <= d1) ? primary : secondary;
            le_add(&out[i], colour, le_mul255(frac, level));
        }
    }
}

static void le_render_split_pulse(struct led_rgb out[LE_LED_PIXELS],
                                  const struct le_music_features *f,
                                  struct led_rgb primary,
                                  struct led_rgb secondary)
{
    unsigned int count = (f->onset_high > f->onset_low) ? 4U : 2U;
    unsigned int step = LE_LED_PIXELS / count;
    unsigned int size = 1U + f->loudness_fast / 80U;
    unsigned int i;

    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int region = i % step;
        unsigned int in_region = region < size;
        unsigned int band;
        if (!in_region)
            continue;
        if (i < 4U)
            band = f->onset_low;
        else if (i < 9U)
            band = f->onset_mid;
        else
            band = f->onset_high;
        le_add(&out[i], (i % 2U == 0U) ? primary : secondary, band);
    }
}

static void le_render_classic_mirror(struct led_rgb out[LE_LED_PIXELS],
                                     const struct le_music_features *f,
                                     struct le_music_render_state *state,
                                     struct led_rgb primary,
                                     struct led_rgb secondary)
{
    unsigned int rotate = (f->beat_phase * 2U) / 65536U;  /* 0..1 */
    unsigned int bands[6];
    unsigned int i;

    bands[0] = f->onset_low;
    bands[1] = f->onset_low / 2U + f->onset_mid / 2U;
    bands[2] = f->onset_mid;
    bands[3] = f->onset_mid / 2U + f->onset_high / 2U;
    bands[4] = f->onset_high;
    bands[5] = f->energy;

    for (i = 0U; i < 6U; i++) {
        unsigned int at = (i + rotate) % 6U;
        unsigned int mirrored = (LE_LED_PIXELS - 1U - at) % LE_LED_PIXELS;
        if (bands[i] > state->peak[i])
            state->peak[i] = bands[i];
        else
            state->peak[i] = state->peak[i] > 12U ? state->peak[i] - 12U : 0U;
        le_add(&out[at], primary, state->peak[i]);
        le_add(&out[mirrored], secondary, state->peak[i]);
    }
}

static void le_render_ember(struct led_rgb out[LE_LED_PIXELS],
                            const struct le_music_features *f,
                            struct le_music_render_state *state,
                            struct led_rgb primary, struct led_rgb secondary)
{
    unsigned int i;

    /* Bass-weighted amber/red motion with substantial black space. */
    for (i = 0U; i < LE_LED_PIXELS; i++) {
        int near = (i < 3U) || (i >= 10U);
        if (near)
            le_add(&out[i], primary, le_mul255(f->loudness_slow, 160U));
        else if (i == 5U || i == 7U)
            le_add(&out[i], secondary, f->onset_low / 2U);
    }
    /* Sparse cool transient sparks. */
    if (f->transientness > 120U) {
        state->free_phase = (f->timestamp_ms / 240U) % LE_LED_PIXELS;
        le_add(&out[state->free_phase], (struct led_rgb){120U, 170U, 255U},
               f->transientness);
    }
}

/* ------------------------------ transitions ----------------------------- */

static void le_render_effect(struct led_rgb out[LE_LED_PIXELS],
                             const struct le_music_scene *scene,
                             struct led_rgb primary,
                             struct led_rgb accent)
{
    unsigned int age = scene->effect_age_ms;
    unsigned int center = scene->effect_center % LE_LED_PIXELS;
    unsigned int i;

    switch (scene->effect) {
    case LE_EFFECT_INHALE_CUT_BLOOM:
        /* 1. contract, 2. dark cut, 3. one near-white lead, 4. bloom. */
        if (age < 500U) {
            unsigned int factor = (500U - age) * 255U / 500U;
            for (i = 0U; i < LE_LED_PIXELS; i++) {
                out[i].r = le_mul255(out[i].r, factor);
                out[i].g = le_mul255(out[i].g, factor);
                out[i].b = le_mul255(out[i].b, factor);
            }
        } else if (age < 700U) {
            for (i = 0U; i < LE_LED_PIXELS; i++)
                out[i] = (struct led_rgb){0U, 0U, 0U};
        } else if (age < 950U) {
            for (i = 0U; i < LE_LED_PIXELS; i++)
                out[i] = (struct led_rgb){0U, 0U, 0U};
            le_set(&out[center], accent, 255U);
        } else {
            unsigned int factor = (age - 950U) * 255U / 550U;
            if (factor > 255U)
                factor = 255U;
            for (i = 0U; i < LE_LED_PIXELS; i++) {
                out[i].r = le_mul255(out[i].r, factor);
                out[i].g = le_mul255(out[i].g, factor);
                out[i].b = le_mul255(out[i].b, factor);
            }
        }
        break;
    case LE_EFFECT_WHITE_APERTURE: {
        unsigned int offset = age * 6U / 900U;
        unsigned int high = age < 300U ? 255U : 120U;
        le_set(&out[(center + offset) % LE_LED_PIXELS], accent, high);
        le_set(&out[(center + 6U + LE_LED_PIXELS - offset) % LE_LED_PIXELS],
               accent, high);
        break;
    }
    case LE_EFFECT_KNIFE_SWEEP: {
        unsigned int pos = age * LE_LED_PIXELS / 800U;
        unsigned int high = age < 200U ? 255U : 90U;
        le_set(&out[pos % LE_LED_PIXELS], accent, high);
        break;
    }
    case LE_EFFECT_SHOCK_RING:
        if (age < 150U) {
            le_set(&out[center], accent, 255U);
            le_set(&out[(center + 6U) % LE_LED_PIXELS], accent, 255U);
        } else {
            unsigned int spread = (age - 150U) * 4U / 550U + 1U;
            for (i = 0U; i < LE_LED_PIXELS; i++) {
                if (le_distance(i, center) <= spread)
                    le_add(&out[i], primary, 180U);
            }
        }
        break;
    default:
        break;
    }
}

/* -------------------------------- entry --------------------------------- */

void le_music_render(const struct le_music_scene *scene,
                     const struct le_music_features *features,
                     struct le_music_render_state *state,
                     struct led_rgb out[LE_LED_PIXELS])
{
    struct led_rgb primary;
    struct led_rgb secondary;
    struct led_rgb accent;
    unsigned int blend = scene->crossfade;
    unsigned int i;

    for (i = 0U; i < LE_LED_PIXELS; i++)
        out[i] = (struct led_rgb){0U, 0U, 0U};

    primary = le_lerp(scene->palette.primary, scene->next_palette.primary,
                      blend);
    secondary = le_lerp(scene->palette.secondary,
                        scene->next_palette.secondary, blend);
    accent = le_lerp(scene->palette.accent, scene->next_palette.accent, blend);

    switch (scene->grammar) {
    case LE_GRAMMAR_RIBBON:
        le_render_ribbon(out, features, primary, secondary, features->warmth);
        break;
    case LE_GRAMMAR_TWIN_ARCS:
        le_render_twin_arcs(out, features, primary, secondary);
        break;
    case LE_GRAMMAR_ORBIT:
        le_render_orbit(out, features, state, primary, secondary);
        break;
    case LE_GRAMMAR_SPLIT_PULSE:
        le_render_split_pulse(out, features, primary, secondary);
        break;
    case LE_GRAMMAR_CLASSIC_MIRROR:
        le_render_classic_mirror(out, features, state, primary, secondary);
        break;
    case LE_GRAMMAR_EMBER:
        le_render_ember(out, features, state, primary, secondary);
        break;
    default:
        break;
    }

    if (scene->effect != LE_EFFECT_NONE)
        le_render_effect(out, scene, primary, accent);
}
