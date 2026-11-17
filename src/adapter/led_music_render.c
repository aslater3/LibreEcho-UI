#include "led_music_render.h"
#include "music_visualizer_protocol.h"

#include <string.h>

/* Sub-pixel ring geometry: motion is kept in 1/65536 px, drawing in 1/256 px.
   Positions wrap at two revolutions so half-speed layers stay continuous. */
#define LE_PX_Q16            65536U
#define LE_RING_Q16          ((unsigned int)LE_LED_PIXELS * LE_PX_Q16)
#define LE_MOTION_WRAP       (2U * LE_RING_Q16)
#define LE_RING8             ((unsigned int)LE_LED_PIXELS * 256U)
/* Tempo is used for speed from this confidence; below it the ring free-runs. */
#define LE_TEMPO_TRUST       64U
/* Rotation: this many pixels per beat, with the tempo folded into one octave
   so half/double-time estimates turn the ring at the same rate. */
#define LE_PX_PER_BEAT       3U
#define LE_BPM_FOLD_LOW      8000U
#define LE_BPM_FOLD_HIGH     16000U
/* Free-running speed in 1/65536 px per ms: 4.5 px/s plus up to 1.5 px/s with
   loudness. */
#define LE_FREE_SPEED        295U
#define LE_FREE_SPEED_LOUD   98U
/* A hit is a rise of this much to at least this level (onsets are published
   normalised: the strongest recent hit is near 255). */
#define LE_HIT_RISE          16U
#define LE_HIT_LEVEL         40U
/* A tracked beat with no kick of its own within this time still pulses. */
#define LE_BEAT_FILL_MS      150U
/* Hit envelopes: instant attack, linear release from full in this time. */
#define LE_KICK_RELEASE_MS   240U
#define LE_SNARE_RELEASE_MS  180U
#define LE_HAT_RELEASE_MS    110U
/* A kick also shoves the rotation: up to this much extra speed (x/256),
   released linearly. */
#define LE_SHOVE_MAX         640U
#define LE_SHOVE_RELEASE_MS  260U
/* Bloom ripples. */
#define LE_RIPPLE_KICK_MS    460U
#define LE_RIPPLE_SNARE_MS   320U
/* The chase reverses on a snare, at most this often. */
#define LE_REVERSE_MIN_MS    450U
/* Longest clock step honoured at once (a stall must not jump the ring). */
#define LE_MAX_STEP_MS       500U

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

/* Circular distance from pixel i to a sub-pixel position (both 1/256 px). */
static unsigned int le_distance8(unsigned int i, unsigned int pos8)
{
    unsigned int at = i * 256U;
    unsigned int d = at > pos8 ? at - pos8 : pos8 - at;
    return d > LE_RING8 / 2U ? LE_RING8 - d : d;
}

static unsigned int le_pos8(unsigned int q16, unsigned int offset_px)
{
    return ((q16 >> 8) + offset_px * 256U) % LE_RING8;
}

void le_music_render_state_reset(struct le_music_render_state *state,
                                 unsigned int session)
{
    if (state == NULL)
        return;
    memset(state, 0, sizeof(*state));
    state->motion_q16 = (session % LE_LED_PIXELS) * LE_PX_Q16;
    state->chase_q16 = state->motion_q16;
    state->bloom_origin = session % LE_LED_PIXELS;
    state->kick_ripple_ms = LE_RIPPLE_KICK_MS;
    state->snare_ripple_ms = LE_RIPPLE_SNARE_MS;
}

static unsigned int le_release(unsigned int value, unsigned int dt,
                               unsigned int full, unsigned int release_ms)
{
    unsigned int fall = dt * full / release_ms;

    return value > fall ? value - fall : 0U;
}

/* Strength of a new hit in one onset group, 0 when there is none: the
   published onset rose clearly, or its event flag was just raised (the
   producer latches event flags across several frames). */
static unsigned int le_hit(unsigned int now, unsigned int last, int flag_rose)
{
    unsigned int strength = 0U;

    /* Even a modest hit must read on the ring: 40 -> 120, 255 -> 255. */
    if (now >= LE_HIT_LEVEL && now > last + LE_HIT_RISE)
        strength = 96U + now * 159U / 255U;
    if (flag_rose && strength < 180U)
        strength = 180U;
    return strength;
}

/*
 * Advance motion and the hit envelopes to clock_ms.  Everything runs on the
 * render clock, so the producer frame rate does not change speeds.
 *
 * The ring always turns: LE_PX_PER_BEAT pixels per beat once a tempo is
 * trusted, a steady free-running speed otherwise.  Hits drive it: a kick
 * punches the kick envelope (brightness and size in every grammar) and shoves
 * the rotation forward, a snare and a hat punch their own envelopes.  A kick
 * and a snare each start a bloom ripple, and a snare reverses the chase.
 */
static void le_render_advance(struct le_music_render_state *s,
                              const struct le_music_features *f,
                              unsigned int clock_ms)
{
    unsigned int dt = 0U;
    unsigned int speed;
    unsigned int step;
    int tempo = f->beat_confidence >= LE_TEMPO_TRUST && f->bpm_x100 >= 3000U;
    int new_frame = !s->have_frame || f->seq != s->last_seq ||
                    f->timestamp_ms != s->last_frame_ms;

    if (!s->have_clock) {
        s->clock_ms = clock_ms;
        s->have_clock = 1;
    } else if (clock_ms - s->clock_ms < 0x80000000U) {
        dt = clock_ms - s->clock_ms;
        s->clock_ms = clock_ms;
    }
    if (dt > LE_MAX_STEP_MS)
        dt = LE_MAX_STEP_MS;
    s->step_ms = dt;

    s->onset_env = le_release(s->onset_env, dt, 255U, LE_KICK_RELEASE_MS);
    s->snare_env = le_release(s->snare_env, dt, 255U, LE_SNARE_RELEASE_MS);
    s->hat_env = le_release(s->hat_env, dt, 255U, LE_HAT_RELEASE_MS);
    s->shove = le_release(s->shove, dt, LE_SHOVE_MAX, LE_SHOVE_RELEASE_MS);
    s->kick_ripple_ms += dt;
    if (s->kick_ripple_ms > LE_RIPPLE_KICK_MS)
        s->kick_ripple_ms = LE_RIPPLE_KICK_MS;
    s->snare_ripple_ms += dt;
    if (s->snare_ripple_ms > LE_RIPPLE_SNARE_MS)
        s->snare_ripple_ms = LE_RIPPLE_SNARE_MS;
    s->since_reverse_ms += dt;
    if (s->since_reverse_ms > LE_REVERSE_MIN_MS)
        s->since_reverse_ms = LE_REVERSE_MIN_MS;
    s->since_kick_ms += dt;
    if (s->since_kick_ms > LE_MAX_STEP_MS)
        s->since_kick_ms = LE_MAX_STEP_MS;

    if (new_frame) {
        unsigned int events = f->events;
        unsigned int rose = s->have_frame ? events & ~s->last_events : 0U;
        unsigned int kick;
        unsigned int snare;
        unsigned int hat;
        unsigned int phase = f->beat_phase & 0xFFFFU;

        if (!s->have_frame) {
            s->last_onset = f->onset_low;
            s->last_mid = f->onset_mid;
            s->last_high = f->onset_high;
            s->last_phase = phase;
        }
        kick = le_hit(f->onset_low, s->last_onset,
                      (rose & LE_MUSIC_EVENT_KICK) != 0U);
        snare = le_hit(f->onset_mid, s->last_mid,
                       (rose & LE_MUSIC_EVENT_SNARE) != 0U);
        hat = le_hit(f->onset_high, s->last_high,
                     (rose & LE_MUSIC_EVENT_HIGH) != 0U);
        /* A tracked beat drives the ring even when its kick is buried in
           the mix: the beat phase wrapped and no kick landed with it. */
        if (kick == 0U && tempo && s->last_phase > phase &&
            s->last_phase - phase > 0x8000U &&
            s->since_kick_ms >= LE_BEAT_FILL_MS) {
            kick = 120U + f->beat_strength / 3U;
            if (kick > 200U)
                kick = 200U;
        }
        if (kick > 0U) {
            s->since_kick_ms = 0U;
            if (kick > s->onset_env)
                s->onset_env = kick;
            if (kick * LE_SHOVE_MAX / 255U > s->shove)
                s->shove = kick * LE_SHOVE_MAX / 255U;
            /* Each bloom opens five pixels on from the last one. */
            s->bloom_origin = (s->bloom_origin + 5U) % LE_LED_PIXELS;
            s->kick_ripple_ms = 0U;
            s->kick_ripple_level = kick;
        }
        if (snare > 0U) {
            if (snare > s->snare_env)
                s->snare_env = snare;
            s->snare_ripple_ms = 0U;
            s->snare_ripple_level = snare;
            if (s->since_reverse_ms >= LE_REVERSE_MIN_MS) {
                s->chase_reverse = !s->chase_reverse;
                s->since_reverse_ms = 0U;
            }
        }
        if (hat > s->hat_env)
            s->hat_env = hat;
        s->last_onset = f->onset_low;
        s->last_mid = f->onset_mid;
        s->last_high = f->onset_high;
        s->last_events = events;
        s->last_phase = phase;
        s->last_seq = f->seq;
        s->last_frame_ms = f->timestamp_ms;
        s->have_frame = 1;
    }

    if (tempo) {
        unsigned int bpm = f->bpm_x100;

        while (bpm < LE_BPM_FOLD_LOW)
            bpm *= 2U;
        while (bpm > LE_BPM_FOLD_HIGH)
            bpm /= 2U;
        /* px/beat * beats/ms, in 1/65536 px. */
        speed = bpm * LE_PX_PER_BEAT * 64U / 5859U;
    } else {
        speed = LE_FREE_SPEED + f->loudness_fast * LE_FREE_SPEED_LOUD / 255U;
    }
    step = dt * (speed * (256U + s->shove) / 256U);
    s->motion_q16 = (s->motion_q16 + step) % LE_MOTION_WRAP;
    if (s->chase_reverse)
        s->chase_q16 = (s->chase_q16 + LE_MOTION_WRAP -
                        step % LE_MOTION_WRAP) % LE_MOTION_WRAP;
    else
        s->chase_q16 = (s->chase_q16 + step) % LE_MOTION_WRAP;
}

/* ------------------------------- grammars ------------------------------- */

/* Resting brightness of a grammar's body; hits add on top of it.  Quiet
   passages dim the whole look instead of leaving a bright idle ring. */
static unsigned int le_body_level(const struct le_music_features *f)
{
    unsigned int level = 56U + f->loudness_fast / 4U;
    unsigned int gate = f->loudness_fast * 6U;

    return gate < 255U ? le_mul255(level, gate) : level;
}

static unsigned int le_punch(unsigned int body, unsigned int env)
{
    unsigned int level = body + env * (255U - body) / 255U;

    return level > 255U ? 255U : level;
}

/* A soft dot: full within core, fading linearly to nothing at half (1/256
   px). */
static void le_dot(struct led_rgb out[LE_LED_PIXELS], unsigned int pos8,
                   unsigned int half, struct led_rgb colour,
                   unsigned int level)
{
    unsigned int core = half > 256U ? half - 256U : 0U;
    unsigned int i;

    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int d = le_distance8(i, pos8);

        if (d >= half)
            continue;
        le_add(&out[i], colour,
               d <= core ? level
                         : le_mul255(level, (half - d) * 255U / (half - core)));
    }
}

/* Comet: a bright head with a fading tail chasing round the ring.  A kick
   whitens the head and stretches the tail, a snare fires a spark opposite. */
static void le_render_ribbon(struct led_rgb out[LE_LED_PIXELS],
                             const struct le_music_features *f,
                             const struct le_music_render_state *state,
                             const struct le_music_palette *pal)
{
    unsigned int head = le_pos8(state->motion_q16, 0U);
    unsigned int kick = state->onset_env;
    unsigned int tail = 768U + kick * 2U;            /* 3..5 px */
    unsigned int level = le_punch(le_body_level(f) + 40U, kick);
    struct led_rgb lead = le_lerp(pal->primary, pal->accent, kick * 3U / 4U);
    unsigned int i;

    for (i = 0U; i < LE_LED_PIXELS; i++) {
        /* Distance behind the head, 0..RING8-1. */
        unsigned int behind = (head + LE_RING8 - i * 256U) % LE_RING8;
        unsigned int ahead = LE_RING8 - behind;

        if (behind < tail) {
            unsigned int frac = (tail - behind) * 255U / tail;
            struct led_rgb colour = le_lerp(pal->secondary, lead, frac);

            le_add(&out[i], colour, le_mul255(le_mul255(frac, frac), level));
        } else if (ahead < 256U) {
            le_add(&out[i], lead, le_mul255(256U - ahead - 1U, level));
        }
    }
    if (state->snare_env > 0U)
        le_dot(out, (head + 6U * 256U) % LE_RING8, 300U, pal->secondary,
               state->snare_env);
}

/* Propeller: two opposite blades turning together.  They swell and flare on
   a kick; a snare tips them with the accent. */
static void le_render_twin_arcs(struct led_rgb out[LE_LED_PIXELS],
                                const struct le_music_features *f,
                                const struct le_music_render_state *state,
                                const struct le_music_palette *pal)
{
    unsigned int kick = state->onset_env;
    unsigned int a = le_pos8(state->motion_q16, 0U);
    unsigned int b = (a + 6U * 256U) % LE_RING8;
    unsigned int half = 300U + kick * 3U / 2U;       /* 1.2..2.7 px */
    unsigned int level = le_punch(le_body_level(f), kick);

    le_dot(out, a, half, pal->primary, level);
    le_dot(out, b, half, pal->secondary, level);
    if (state->snare_env > 0U) {
        le_dot(out, a, 200U, pal->accent, state->snare_env);
        le_dot(out, b, 200U, pal->accent, state->snare_env);
    }
}

/* Counter-rotation: two dots circle in opposite directions and flare where
   they cross.  A kick swells both, a hat glints between them. */
static void le_render_orbit(struct led_rgb out[LE_LED_PIXELS],
                            const struct le_music_features *f,
                            const struct le_music_render_state *state,
                            const struct le_music_palette *pal)
{
    unsigned int kick = state->onset_env;
    unsigned int a = le_pos8(state->motion_q16, 0U);
    unsigned int b = (LE_RING8 - a) % LE_RING8;
    unsigned int half = 330U + kick;                 /* 1.3..2.3 px */
    unsigned int level = le_punch(le_body_level(f) + 20U, kick);

    le_dot(out, a, half, pal->primary, level);
    le_dot(out, b, half, pal->secondary, level);
    if (state->hat_env > 0U)
        le_dot(out, (a + 3U * 256U) % LE_RING8, 200U, pal->accent,
               state->hat_env * 2U / 3U);
}

static void le_ripple(struct led_rgb out[LE_LED_PIXELS], unsigned int origin8,
                      unsigned int age, unsigned int life, unsigned int reach,
                      struct led_rgb from, struct led_rgb to,
                      unsigned int level)
{
    unsigned int radius;
    unsigned int fade;
    struct led_rgb colour;

    if (age >= life)
        return;
    radius = age * reach / life;
    fade = (life - age) * 255U / life;
    colour = le_lerp(from, to, 255U - fade);
    level = le_mul255(level, fade);
    if (radius < 128U) {
        le_dot(out, origin8, 330U, colour, level);
        return;
    }
    le_dot(out, (origin8 + radius) % LE_RING8, 330U, colour, level);
    le_dot(out, (origin8 + LE_RING8 - radius) % LE_RING8, 330U, colour,
           level);
}

/* Bloom: every kick opens a ripple that runs both ways round the ring from a
   point that hops on each hit; a snare answers from the far side.  A faint
   pilot dot keeps turning between hits. */
static void le_render_split_pulse(struct led_rgb out[LE_LED_PIXELS],
                                  const struct le_music_features *f,
                                  const struct le_music_render_state *state,
                                  const struct le_music_palette *pal)
{
    unsigned int origin = state->bloom_origin * 256U;

    le_dot(out, le_pos8(state->motion_q16, 0U), 300U, pal->secondary,
           le_body_level(f) / 2U);
    le_ripple(out, origin, state->kick_ripple_ms, LE_RIPPLE_KICK_MS,
              5U * 256U + 128U, pal->accent, pal->primary,
              le_punch(120U, state->kick_ripple_level));
    le_ripple(out, (origin + 6U * 256U) % LE_RING8, state->snare_ripple_ms,
              LE_RIPPLE_SNARE_MS, 3U * 256U, pal->secondary, pal->secondary,
              le_punch(60U, state->snare_ripple_level));
}

/* Mirrored meter: a bar grows both ways from an axis that turns with the
   ring.  It rests as a dot and slams out on a kick; a snare lights the
   tips. */
static void le_render_classic_mirror(struct led_rgb out[LE_LED_PIXELS],
                                     const struct le_music_features *f,
                                     const struct le_music_render_state *state,
                                     const struct le_music_palette *pal)
{
    unsigned int axis = le_pos8(state->motion_q16, 0U);
    unsigned int kick = state->onset_env;
    /* Half-length under a pixel at rest, up to 4.5 px on a kick. */
    unsigned int reach = 160U + f->loudness_fast / 3U + kick * 7U / 2U;
    unsigned int level = le_punch(le_body_level(f) + 20U, kick);
    unsigned int i;

    if (reach > 1152U)
        reach = 1152U;
    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int d = le_distance8(i, axis);
        unsigned int cover;

        if (d >= reach + 128U)
            continue;
        cover = d + 128U <= reach ? 255U : (reach + 128U - d) * 255U / 256U;
        le_add(&out[i], le_lerp(pal->primary, pal->secondary,
                                d * 255U / 1536U),
               le_mul255(cover, level));
        if (state->snare_env > 0U && d + 256U > reach)
            le_add(&out[i], pal->accent,
                   le_mul255(cover, state->snare_env * 2U / 3U));
    }
}

/* Chase: three dots a third of the ring apart running round together.  A
   kick flares them, a snare throws the chase into reverse. */
static void le_render_ember(struct led_rgb out[LE_LED_PIXELS],
                            const struct le_music_features *f,
                            const struct le_music_render_state *state,
                            const struct le_music_palette *pal)
{
    unsigned int kick = state->onset_env;
    unsigned int at = le_pos8(state->chase_q16, 0U);
    unsigned int half = 280U + kick / 2U;            /* 1.1..1.6 px */
    unsigned int level = le_punch(le_body_level(f) + 10U, kick);

    le_dot(out, at, half, pal->primary, level);
    le_dot(out, (at + 4U * 256U) % LE_RING8, half, pal->secondary, level);
    le_dot(out, (at + 8U * 256U) % LE_RING8, half,
           le_lerp(pal->primary, pal->accent, state->hat_env), level);
}

static void le_render_grammar(struct led_rgb out[LE_LED_PIXELS],
                              enum le_music_grammar grammar,
                              const struct le_music_features *f,
                              const struct le_music_render_state *state,
                              const struct le_music_palette *pal)
{
    switch (grammar) {
    case LE_GRAMMAR_RIBBON:
        le_render_ribbon(out, f, state, pal);
        break;
    case LE_GRAMMAR_TWIN_ARCS:
        le_render_twin_arcs(out, f, state, pal);
        break;
    case LE_GRAMMAR_ORBIT:
        le_render_orbit(out, f, state, pal);
        break;
    case LE_GRAMMAR_SPLIT_PULSE:
        le_render_split_pulse(out, f, state, pal);
        break;
    case LE_GRAMMAR_CLASSIC_MIRROR:
        le_render_classic_mirror(out, f, state, pal);
        break;
    case LE_GRAMMAR_EMBER:
        le_render_ember(out, f, state, pal);
        break;
    default:
        break;
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
    le_music_render_at(scene, features, state, features->timestamp_ms, out);
}

void le_music_render_at(const struct le_music_scene *scene,
                        const struct le_music_features *features,
                        struct le_music_render_state *state,
                        unsigned int clock_ms,
                        struct led_rgb out[LE_LED_PIXELS])
{
    struct led_rgb primary;
    struct led_rgb accent;
    unsigned int blend = scene->crossfade;
    unsigned int i;

    for (i = 0U; i < LE_LED_PIXELS; i++)
        out[i] = (struct led_rgb){0U, 0U, 0U};

    le_render_advance(state, features, clock_ms);
    le_render_grammar(out, scene->grammar, features, state, &scene->palette);
    /* A scene change dissolves from one look into the next. */
    if (blend > 0U) {
        struct led_rgb next[LE_LED_PIXELS];

        for (i = 0U; i < LE_LED_PIXELS; i++)
            next[i] = (struct led_rgb){0U, 0U, 0U};
        le_render_grammar(next, scene->next_grammar, features, state,
                          &scene->next_palette);
        for (i = 0U; i < LE_LED_PIXELS; i++)
            out[i] = le_lerp(out[i], next[i], blend);
    }

    primary = le_lerp(scene->palette.primary, scene->next_palette.primary,
                      blend);
    accent = le_lerp(scene->palette.accent, scene->next_palette.accent, blend);
    if (scene->effect != LE_EFFECT_NONE)
        le_render_effect(out, scene, primary, accent);
}
