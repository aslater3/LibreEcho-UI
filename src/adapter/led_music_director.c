#include "led_music_director.h"
#include "music_visualizer_protocol.h"

#include <string.h>

#define LE_LOW_CONFIDENCE      90U
#define LE_SECTION_NOVELTY     150U
#define LE_BUILD_MIN           130U
#define LE_EVENT_STRENGTH_MIN  110U
/* A quiet gap counts as structure after this long (was 8 frames at the
   original ~11.7 fps producer rate). */
#define LE_QUIET_GAP_SECONDS   0.68
/* EMA time constant: a quarter step per ~85 ms frame at 11.7 fps. */
#define LE_EMA_SECONDS         0.256
#define LE_STALE_DWELLS        1.2

/* Mood follows the music over this time constant. */
#define LE_MOOD_SECONDS        4.0
/* The opening scene is short: its palette was chosen before any music. */
#define LE_FIRST_DWELL_SECONDS 3.0
/* A mood is left only when the score passes its edge by this much. */
#define LE_MOOD_HYSTERESIS     6.0
/* Shortest scene a change of mood may cut short. */
#define LE_MOOD_DWELL_SECONDS  4.0
#define LE_MOOD_PALETTES       3U
/* Below this loudness a frame is silence and leaves the mood alone. */
#define LE_MOOD_MIN_LOUDNESS   48U

/* Lower heat edge of balanced, energetic and intense. */
static const double le_mood_edges[LE_MOOD_COUNT - 1] = {20.0, 45.0, 70.0};

static const struct {
    struct led_rgb primary;
    struct led_rgb secondary;
    int accent;                 /* enum led_accent */
} le_palette_families[LE_MOOD_COUNT][LE_MOOD_PALETTES] = {
    {   /* calm: blues and teals */
        {{ 20U, 110U, 255U}, {  0U, 210U, 190U}, LE_ACCENT_WARM_WHITE},
        {{  0U, 170U, 255U}, { 90U, 110U, 255U}, LE_ACCENT_WARM_WHITE},
        {{  0U, 200U, 150U}, { 30U, 120U, 255U}, LE_ACCENT_WARM_WHITE}
    },
    {   /* balanced: violet, green, amber */
        {{130U,  60U, 255U}, {  0U, 200U, 220U}, LE_ACCENT_WARM_WHITE},
        {{ 60U, 220U,  90U}, {  0U, 170U, 255U}, LE_ACCENT_WARM_WHITE},
        {{255U, 170U,  30U}, {150U,  70U, 255U}, LE_ACCENT_COOL_WHITE}
    },
    {   /* energetic: neon pink, yellow, cyan */
        {{255U,  20U, 140U}, {  0U, 190U, 255U}, LE_ACCENT_WARM_WHITE},
        {{255U, 200U,   0U}, {255U,  30U, 120U}, LE_ACCENT_COOL_WHITE},
        {{160U,  40U, 255U}, {255U,  60U,  60U}, LE_ACCENT_COOL_WHITE}
    },
    {   /* intense: reds and oranges */
        {{255U,  20U,   0U}, {255U, 120U,   0U}, LE_ACCENT_COOL_WHITE},
        {{255U,  60U,   0U}, {255U, 190U,  20U}, LE_ACCENT_COOL_WHITE},
        {{255U,   0U,  30U}, {255U,  90U,   0U}, LE_ACCENT_COOL_WHITE}
    }
};

static unsigned int le_rng_next(unsigned int *state)
{
    unsigned int x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x ? x : 0x9E3779B9U;
    return *state;
}

const char *le_music_grammar_name(enum le_music_grammar grammar)
{
    switch (grammar) {
    case LE_GRAMMAR_RIBBON: return "ribbon";
    case LE_GRAMMAR_TWIN_ARCS: return "twin_arcs";
    case LE_GRAMMAR_ORBIT: return "orbit";
    case LE_GRAMMAR_SPLIT_PULSE: return "split_pulse";
    case LE_GRAMMAR_CLASSIC_MIRROR: return "classic_mirror";
    case LE_GRAMMAR_EMBER: return "ember";
    default: return "none";
    }
}

const char *le_music_mood_name(enum le_music_mood mood)
{
    switch (mood) {
    case LE_MOOD_CALM: return "calm";
    case LE_MOOD_BALANCED: return "balanced";
    case LE_MOOD_ENERGETIC: return "energetic";
    case LE_MOOD_INTENSE: return "intense";
    default: return "none";
    }
}

const char *le_music_effect_name(enum le_music_effect effect)
{
    switch (effect) {
    case LE_EFFECT_INHALE_CUT_BLOOM: return "inhale_cut_bloom";
    case LE_EFFECT_WHITE_APERTURE: return "white_aperture";
    case LE_EFFECT_KNIFE_SWEEP: return "knife_sweep";
    case LE_EFFECT_SHOCK_RING: return "shock_ring";
    default: return "none";
    }
}

void le_music_director_config_defaults(struct le_music_director_config *cfg)
{
    if (cfg == NULL)
        return;
    cfg->scene_dwell_seconds = 10.0;
    cfg->scene_crossfade_seconds = 1.0;
    cfg->effect_cooldown_seconds = 8.0;
    cfg->effect_duration_seconds = 1.5;
}

/*
 * Mood score of one frame: loudness plus the spectral tilt towards the upper
 * bands.  The caller averages it over seconds; the fast loudness is used
 * because the engine's slow one takes seconds to rise when a session starts.  Loud, bright, dense material (distorted guitars, driving
 * electronic music) scores high; quiet or bass-weighted material scores low.
 */
static double le_frame_heat(const struct le_music_features *f)
{
    return ((double)f->loudness_fast - 110.0) +
           1.5 * ((double)f->brightness_axis - (double)f->warmth);
}

static enum le_music_mood le_mood_for(const struct le_music_director *d)
{
    unsigned int mood = (unsigned int)d->mood;

    while (mood + 1U < (unsigned int)LE_MOOD_COUNT &&
           d->heat >= le_mood_edges[mood] + LE_MOOD_HYSTERESIS)
        mood++;
    while (mood > 0U && d->heat < le_mood_edges[mood - 1U] - LE_MOOD_HYSTERESIS)
        mood--;
    return (enum le_music_mood)mood;
}

/* The next palette: the current mood's family, never the one showing. */
static struct le_music_palette le_palette_for(struct le_music_director *d)
{
    struct le_music_palette palette;
    enum le_music_mood mood = le_mood_for(d);
    unsigned int index = le_rng_next(&d->rng) % LE_MOOD_PALETTES;
    struct led_rgb accent;

    if (mood == d->mood && index == d->palette_index)
        index = (index + 1U) % LE_MOOD_PALETTES;
    d->mood = mood;
    d->palette_index = index;
    palette.primary = le_palette_families[mood][index].primary;
    palette.secondary = le_palette_families[mood][index].secondary;
    accent = palette.primary;
    (void)led_output_accent(
        (enum led_accent)le_palette_families[mood][index].accent, &accent);
    palette.accent = accent;
    return palette;
}

void le_music_director_reset(struct le_music_director *director,
                             unsigned int session,
                             const struct le_music_director_config *cfg)
{
    if (director == NULL)
        return;
    memset(director, 0, sizeof(*director));
    le_music_director_config_defaults(&director->cfg);
    if (cfg != NULL)
        director->cfg = *cfg;
    director->session = session;
    director->have_session = 1;
    /* Deterministic per-session variation. */
    director->rng = session * 2654435761U + 0x9E3779B9U;
    if (director->rng == 0U)
        director->rng = 1U;
    director->grammar = (enum le_music_grammar)
        (le_rng_next(&director->rng) % (unsigned int)LE_GRAMMAR_COUNT);
    director->pending_grammar = director->grammar;
    director->mood = LE_MOOD_BALANCED;
    director->heat = le_mood_edges[0] + LE_MOOD_HYSTERESIS;
    director->heat_seconds = 0.0;
    director->palette_index = LE_MOOD_PALETTES;
    director->palette = le_palette_for(director);
    director->pending_palette = director->palette;
    director->effect = LE_EFFECT_NONE;
}

static enum le_music_grammar le_pick_grammar(struct le_music_director *d)
{
    enum le_music_grammar chosen;
    unsigned int attempt;

    /* Every grammar moves on its own clock and reacts to hits, so none
       needs a beat lock. */
    for (attempt = 0U; attempt < 8U; attempt++) {
        chosen = (enum le_music_grammar)
            (le_rng_next(&d->rng) % (unsigned int)LE_GRAMMAR_COUNT);
        if (chosen != d->grammar)
            return chosen;
    }
    return d->grammar == LE_GRAMMAR_RIBBON ? LE_GRAMMAR_TWIN_ARCS
                                           : LE_GRAMMAR_RIBBON;
}

static void le_begin_change(struct le_music_director *d,
                            const struct le_music_features *f, double now)
{
    d->pending_grammar = le_pick_grammar(d);
    (void)f;
    d->pending_palette = le_palette_for(d);
    d->crossfading = 1;
    d->change_started = now;
}

static void le_start_effect(struct le_music_director *d,
                            enum le_music_effect effect,
                            const struct le_music_features *f, double now)
{
    d->effect = effect;
    d->effect_started = now;
    d->last_effect = now;
    d->have_effect = 1;
    d->effect_center = (f->beat_phase * (unsigned int)LE_LED_PIXELS) / 65536U;
    if (d->effect_center >= LE_LED_PIXELS)
        d->effect_center = 0U;
}

static int le_effect_ready(const struct le_music_director *d, double now)
{
    if (d->effect != LE_EFFECT_NONE)
        return 0;
    if (!d->have_effect)
        return 1;
    return (now - d->last_effect) >= d->cfg.effect_cooldown_seconds;
}

static void le_consider_effect(struct le_music_director *d,
                               const struct le_music_features *f, double now)
{
    unsigned int events = f->events;
    int had_build = d->build_ema >= LE_BUILD_MIN ||
                    f->build >= LE_BUILD_MIN ||
                    (events & LE_MUSIC_EVENT_BUILD) ||
                    (events & LE_MUSIC_EVENT_FILL) ||
                    (d->quiet &&
                     (now - d->quiet_since) >= LE_QUIET_GAP_SECONDS);

    if (!le_effect_ready(d, now))
        return;
    if ((events & (LE_MUSIC_EVENT_SECTION | LE_MUSIC_EVENT_DROP |
                   LE_MUSIC_EVENT_REENTRY | LE_MUSIC_EVENT_FILL |
                   LE_MUSIC_EVENT_BUILD)) == 0U)
        return;

    if ((events & (LE_MUSIC_EVENT_DROP | LE_MUSIC_EVENT_REENTRY)) &&
        had_build &&
        f->event_strength >= LE_EVENT_STRENGTH_MIN &&
        f->beat_confidence >= LE_LOW_CONFIDENCE) {
        le_start_effect(d, LE_EFFECT_INHALE_CUT_BLOOM, f, now);
        return;
    }
    if ((events & LE_MUSIC_EVENT_SECTION) &&
        f->novelty >= LE_SECTION_NOVELTY &&
        f->beat_confidence >= 100U) {
        le_start_effect(d, LE_EFFECT_WHITE_APERTURE, f, now);
        return;
    }
    if ((events & LE_MUSIC_EVENT_FILL) &&
        f->event_strength >= 100U &&
        f->transientness >= 110U) {
        le_start_effect(d, LE_EFFECT_KNIFE_SWEEP, f, now);
        return;
    }
    if ((events & LE_MUSIC_EVENT_DROP) &&
        f->event_strength >= 180U &&
        f->beat_confidence >= 110U) {
        le_start_effect(d, LE_EFFECT_SHOCK_RING, f, now);
        return;
    }
}

void le_music_director_update(struct le_music_director *d,
                              const struct le_music_features *f, double now)
{
    if (d == NULL || f == NULL)
        return;

    if (!d->have_history) {
        d->confidence_ema = f->beat_confidence;
        d->build_ema = f->build;
        d->have_history = 1;
        d->heat_seconds = 0.0;
        d->first_update = now;
        d->scene_started = now;
        d->have_scene_time = 1;
    } else {
        /* Same wall-time response at any producer frame rate. */
        double dt = now - d->last_update;
        double a;
        if (dt < 0.0)
            dt = 0.0;
        /* Silence says nothing about the mood, and the first seconds of
           music are a plain running mean so a quiet lead-in does not hold
           the colours back. */
        if (dt > 0.0 && f->loudness_fast >= LE_MOOD_MIN_LOUDNESS) {
            double tau = d->heat_seconds < LE_MOOD_SECONDS
                             ? d->heat_seconds : LE_MOOD_SECONDS;

            d->heat += (le_frame_heat(f) - d->heat) * (dt / (dt + tau));
            d->heat_seconds += dt;
        }
        a = dt / (dt + LE_EMA_SECONDS);
        d->confidence_ema = (unsigned int)
            ((double)d->confidence_ema +
             ((double)f->beat_confidence - (double)d->confidence_ema) * a + 0.5);
        d->build_ema = (unsigned int)
            ((double)d->build_ema +
             ((double)f->build - (double)d->build_ema) * a + 0.5);
    }
    d->last_update = now;

    /* Expire a running transition. */
    if (d->effect != LE_EFFECT_NONE &&
        (now - d->effect_started) >= d->cfg.effect_duration_seconds)
        d->effect = LE_EFFECT_NONE;

    /* Consider transitions with the quiet history accumulated so far. */
    le_consider_effect(d, f, now);

    if (f->loudness_fast < 40U || f->energy < 50U) {
        if (!d->quiet) {
            d->quiet = 1;
            d->quiet_since = now;
        }
    } else {
        d->quiet = 0;
    }

    /* Scene changes: dwell-gated, primarily structural. */
    if (!d->crossfading) {
        int structural = (f->events & (LE_MUSIC_EVENT_SECTION |
                                       LE_MUSIC_EVENT_DROP |
                                       LE_MUSIC_EVENT_REENTRY)) != 0U ||
                         f->novelty >= LE_SECTION_NOVELTY;
        int dwell_ok = d->have_scene_time &&
                       (now - d->scene_started) >= d->cfg.scene_dwell_seconds;
        /* Refresh soon after the dwell even without structural evidence:
           two dwells held one look for ~21 s on real music. */
        int stale = d->have_scene_time &&
                    (now - d->scene_started) >=
                        d->cfg.scene_dwell_seconds * LE_STALE_DWELLS;

        /* The opening palette predates the music: move on once the mood
           has had time to settle. */
        int opening = d->scene_started == d->first_update &&
                      d->heat_seconds >= LE_FIRST_DWELL_SECONDS;

        /* A change of mood (quiet verse into a loud chorus) recolours the
           ring without waiting for the dwell. */
        int mood_shift = d->have_scene_time &&
                         (now - d->scene_started) >= LE_MOOD_DWELL_SECONDS &&
                         le_mood_for(d) != d->mood;

        if ((dwell_ok && structural) || stale || opening || mood_shift)
            le_begin_change(d, f, now);
    } else if ((now - d->change_started) >= d->cfg.scene_crossfade_seconds) {
        d->grammar = d->pending_grammar;
        d->palette = d->pending_palette;
        d->crossfading = 0;
        d->scene_started = now;
    }

    d->prev_events = f->events;
}

void le_music_director_scene(const struct le_music_director *d, double now,
                             struct le_music_scene *scene)
{
    if (d == NULL || scene == NULL)
        return;
    scene->grammar = d->grammar;
    scene->next_grammar = d->pending_grammar;
    scene->palette = d->palette;
    scene->next_palette = d->pending_palette;
    scene->crossfade = 0U;
    if (d->crossfading && d->cfg.scene_crossfade_seconds > 0.0) {
        double progress = (now - d->change_started) /
                          d->cfg.scene_crossfade_seconds;
        if (progress < 0.0)
            progress = 0.0;
        if (progress > 1.0)
            progress = 1.0;
        scene->crossfade = (unsigned int)(progress * 255.0 + 0.5);
    }
    scene->effect = d->effect;
    scene->effect_age_ms = 0U;
    scene->effect_center = d->effect_center;
    if (d->effect != LE_EFFECT_NONE) {
        double age = (now - d->effect_started) * 1000.0;
        scene->effect_age_ms = age < 0.0 ? 0U : (unsigned int)age;
    }
}

void le_music_director_interrupt(struct le_music_director *d)
{
    if (d == NULL)
        return;
    /* Abort without replaying: the scene stays, the effect is gone, and the
       cooldown is armed so a stale event cannot immediately restart it. */
    if (d->effect != LE_EFFECT_NONE) {
        d->effect = LE_EFFECT_NONE;
        d->last_effect = d->effect_started;
        d->have_effect = 1;
    }
}
