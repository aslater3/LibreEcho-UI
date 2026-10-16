/*
 * Portable music scene director (issues #64 and #65).
 *
 * Consumes the frozen v2 feature frame and decides, deterministically:
 *   - which visual grammar is active, with a >=10 s dwell and ~1 s crossfade;
 *   - the restrained two-colour palette (plus accent) for that grammar;
 *   - whether a confidence-gated major transition is running.
 *
 * There is no wall-clock decoration: a major effect is triggered only by
 * structural evidence (build/fill/section/drop/reentry), a confidence floor,
 * and a global cooldown.  Ordinary beats stay inside the active grammar.
 * State is session-seeded so the same feature trace renders identically.
 */
#ifndef LE_MUSIC_DIRECTOR_H
#define LE_MUSIC_DIRECTOR_H

#include "led_output.h"
#include "music_visualizer_protocol.h"

enum le_music_grammar {
    LE_GRAMMAR_RIBBON = 0,
    LE_GRAMMAR_TWIN_ARCS,
    LE_GRAMMAR_ORBIT,
    LE_GRAMMAR_SPLIT_PULSE,
    LE_GRAMMAR_CLASSIC_MIRROR,
    LE_GRAMMAR_EMBER,
    LE_GRAMMAR_COUNT
};

enum le_music_effect {
    LE_EFFECT_NONE = 0,
    LE_EFFECT_INHALE_CUT_BLOOM,
    LE_EFFECT_WHITE_APERTURE,
    LE_EFFECT_KNIFE_SWEEP,
    LE_EFFECT_SHOCK_RING
};

struct le_music_palette {
    struct led_rgb primary;
    struct led_rgb secondary;
    struct led_rgb accent;
};

struct le_music_director_config {
    double scene_dwell_seconds;      /* default 10.0 */
    double scene_crossfade_seconds;  /* default 1.0 */
    double effect_cooldown_seconds;  /* default 8.0 */
    double effect_duration_seconds;  /* default 1.5 */
};

struct le_music_director {
    struct le_music_director_config cfg;
    unsigned int session;
    int have_session;
    unsigned int rng;

    enum le_music_grammar grammar;
    enum le_music_grammar pending_grammar;
    struct le_music_palette palette;
    struct le_music_palette pending_palette;
    int crossfading;
    double scene_started;
    double change_started;
    int have_scene_time;

    unsigned int confidence_ema;
    unsigned int build_ema;
    unsigned int quiet_frames;
    int have_history;

    enum le_music_effect effect;
    double effect_started;
    double last_effect;
    int have_effect;
    unsigned int effect_center;

    unsigned int prev_events;
};

/* What the renderer reads on a frame. */
struct le_music_scene {
    enum le_music_grammar grammar;        /* outgoing/current grammar */
    enum le_music_grammar next_grammar;   /* incoming during a crossfade */
    struct le_music_palette palette;
    struct le_music_palette next_palette;
    unsigned int crossfade;               /* 0..255, 0 == settled */
    enum le_music_effect effect;
    unsigned int effect_age_ms;
    unsigned int effect_center;
};

void le_music_director_config_defaults(struct le_music_director_config *cfg);
void le_music_director_reset(struct le_music_director *director,
                             unsigned int session,
                             const struct le_music_director_config *cfg);
/* Advance the director from one accepted feature frame. */
void le_music_director_update(struct le_music_director *director,
                              const struct le_music_features *features,
                              double now);
/* Snapshot the renderable scene. */
void le_music_director_scene(const struct le_music_director *director,
                             double now, struct le_music_scene *scene);
/*
 * Abort any running transition without replaying it (e.g. a higher-priority
 * owner took the ring).  The base scene is preserved.
 */
void le_music_director_interrupt(struct le_music_director *director);

const char *le_music_grammar_name(enum le_music_grammar grammar);
const char *le_music_effect_name(enum le_music_effect effect);

#endif /* LE_MUSIC_DIRECTOR_H */
