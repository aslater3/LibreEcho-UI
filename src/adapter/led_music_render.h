/*
 * Twelve-pixel music renderer.
 *
 * Turns a director scene plus the current feature frame into a logical
 * 12-pixel RGB frame.  Every grammar shares one interface so the director can
 * crossfade between them.  The ring always turns, at the tempo when one is
 * known; kicks, snares and hats punch brightness and size and shove the
 * rotation.  The palette stays restrained and dark space is preserved: each
 * grammar lights a few pixels and starts from black each frame.
 *
 * The output is a logical frame (brightness is applied later by led_output).
 * Near-white accents use the calibrated named warm/cool points from
 * led_output, never raw 255,255,255.
 */
#ifndef LE_MUSIC_RENDER_H
#define LE_MUSIC_RENDER_H

#include "led_music_director.h"
#include "led_output.h"

struct le_music_render_state {
    /* Motion and hit envelopes, advanced by the render clock so the ring
       keeps moving and hits decay between producer frames. */
    unsigned int clock_ms;             /* last render clock */
    int have_clock;
    unsigned int last_seq;             /* last feature frame seen */
    unsigned int last_frame_ms;
    int have_frame;
    unsigned int motion_q16;           /* ring position, 1 px == 65536 */
    unsigned int chase_q16;            /* same, reversible (chase) */
    int chase_reverse;
    unsigned int since_reverse_ms;
    unsigned int shove;                /* kick speed-up, x/256 */
    unsigned int onset_env;            /* kick envelope 0..255 */
    unsigned int snare_env;
    unsigned int hat_env;
    unsigned int last_onset;           /* last frame onset_low */
    unsigned int last_mid;
    unsigned int last_high;
    unsigned int last_events;          /* last frame event flags */
    unsigned int last_phase;           /* last frame beat phase */
    unsigned int since_kick_ms;        /* time since the last kick or beat */
    unsigned int bloom_origin;         /* pixel the kick ripple opens from */
    unsigned int kick_ripple_ms;       /* ripple ages */
    unsigned int kick_ripple_level;
    unsigned int snare_ripple_ms;
    unsigned int snare_ripple_level;
    unsigned int step_ms;              /* clock advance of this render */
};

void le_music_render_state_reset(struct le_music_render_state *state,
                                 unsigned int session);

/* Render at the frame's own timestamp (one render per frame). */
void le_music_render(const struct le_music_scene *scene,
                     const struct le_music_features *features,
                     struct le_music_render_state *state,
                     struct led_rgb out[LE_LED_PIXELS]);

/*
 * Render at clock_ms, the producer timeline extended by the time since the
 * last accepted frame.  ledd re-renders the last frame every 33 ms; passing
 * the extended clock lets motion and the onset release run between frames.
 * The clock is treated as monotonic: a value behind the last one holds.
 */
void le_music_render_at(const struct le_music_scene *scene,
                        const struct le_music_features *features,
                        struct le_music_render_state *state,
                        unsigned int clock_ms,
                        struct led_rgb out[LE_LED_PIXELS]);

#endif /* LE_MUSIC_RENDER_H */
