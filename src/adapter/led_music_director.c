#include "led_music_director.h"
#include "music_visualizer_protocol.h"

#include <string.h>

#define LE_LOW_CONFIDENCE      90U
#define LE_SECTION_NOVELTY     150U
#define LE_BUILD_MIN           130U
#define LE_EVENT_STRENGTH_MIN  110U
#define LE_QUIET_GAP_FRAMES    8U

static const struct {
    struct led_rgb primary;
    struct led_rgb secondary;
    int accent;                 /* enum led_accent */
} le_palette_families[] = {
    {{ 40U, 120U, 220U}, { 20U, 200U, 190U}, LE_ACCENT_WARM_WHITE},
    {{255U, 120U,  30U}, {255U,  70U,  90U}, LE_ACCENT_COOL_WHITE},
    {{120U,  70U, 230U}, {210U,  60U, 180U}, LE_ACCENT_WARM_WHITE},
    {{255U,  80U,  10U}, {150U,  30U,  20U}, LE_ACCENT_COOL_WHITE}
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

static struct le_music_palette le_palette_for(struct le_music_director *d,
                                              unsigned int warmth)
{
    struct le_music_palette palette;
    unsigned int family;
    struct led_rgb accent;

    if (warmth < 100U)
        family = (le_rng_next(&d->rng) & 1U) ? 0U : 2U;
    else if (warmth <= 150U)
        family = 2U;
    else
        family = (le_rng_next(&d->rng) & 1U) ? 1U : 3U;

    palette.primary = le_palette_families[family].primary;
    palette.secondary = le_palette_families[family].secondary;
    accent = le_palette_families[family].primary;
    (void)led_output_accent((enum led_accent)le_palette_families[family].accent,
                            &accent);
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
    director->palette = le_palette_for(director, 128U);
    director->pending_palette = director->palette;
    director->effect = LE_EFFECT_NONE;
}

static enum le_music_grammar le_pick_grammar(struct le_music_director *d,
                                             int low_confidence)
{
    static const enum le_music_grammar free_run[3] = {
        LE_GRAMMAR_RIBBON, LE_GRAMMAR_TWIN_ARCS, LE_GRAMMAR_EMBER
    };
    enum le_music_grammar chosen;
    unsigned int attempt;

    for (attempt = 0U; attempt < 8U; attempt++) {
        if (low_confidence)
            chosen = free_run[le_rng_next(&d->rng) % 3U];
        else
            chosen = (enum le_music_grammar)
                (le_rng_next(&d->rng) % (unsigned int)LE_GRAMMAR_COUNT);
        if (chosen != d->grammar)
            return chosen;
    }
    return LE_GRAMMAR_RIBBON;
}

static void le_begin_change(struct le_music_director *d,
                            const struct le_music_features *f, double now)
{
    int low_confidence = d->confidence_ema < LE_LOW_CONFIDENCE;

    d->pending_grammar = le_pick_grammar(d, low_confidence);
    d->pending_palette = le_palette_for(d, f->warmth);
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
                    d->quiet_frames >= LE_QUIET_GAP_FRAMES;

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
        d->scene_started = now;
        d->have_scene_time = 1;
    } else {
        d->confidence_ema = (d->confidence_ema * 3U + f->beat_confidence) / 4U;
        d->build_ema = (d->build_ema * 3U + f->build) / 4U;
    }

    /* Expire a running transition. */
    if (d->effect != LE_EFFECT_NONE &&
        (now - d->effect_started) >= d->cfg.effect_duration_seconds)
        d->effect = LE_EFFECT_NONE;

    /* Consider transitions with the quiet history accumulated so far. */
    le_consider_effect(d, f, now);

    if (f->loudness_fast < 40U || f->energy < 50U) {
        if (d->quiet_frames < 10000U)
            d->quiet_frames++;
    } else {
        d->quiet_frames = 0U;
    }

    /* Scene changes: dwell-gated, primarily structural. */
    if (!d->crossfading) {
        int structural = (f->events & (LE_MUSIC_EVENT_SECTION |
                                       LE_MUSIC_EVENT_DROP |
                                       LE_MUSIC_EVENT_REENTRY)) != 0U ||
                         f->novelty >= LE_SECTION_NOVELTY;
        int dwell_ok = d->have_scene_time &&
                       (now - d->scene_started) >= d->cfg.scene_dwell_seconds;
        int stale = d->have_scene_time &&
                    (now - d->scene_started) >=
                        d->cfg.scene_dwell_seconds * 2.0;
        int low_conf_orbit = d->grammar == LE_GRAMMAR_ORBIT &&
                             d->confidence_ema < LE_LOW_CONFIDENCE;

        if ((dwell_ok && structural) || stale || low_conf_orbit)
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
