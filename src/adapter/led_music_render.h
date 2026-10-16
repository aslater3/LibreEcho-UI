/*
 * Twelve-pixel music renderer.
 *
 * Turns a director scene plus the current feature frame into a logical
 * 12-pixel RGB frame.  Every grammar shares one interface so the director can
 * crossfade between them.  Rhythm controls motion, timbre controls colour,
 * dynamics control occupied area; the palette stays restrained and dark space
 * is preserved by starting from black each frame.
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
    unsigned int peak[LE_LED_PIXELS];  /* classic mirror peak hold */
    unsigned int free_phase;           /* free-running orbit position */
    int have_phase;
};

void le_music_render_state_reset(struct le_music_render_state *state,
                                 unsigned int session);

void le_music_render(const struct le_music_scene *scene,
                     const struct le_music_features *features,
                     struct le_music_render_state *state,
                     struct led_rgb out[LE_LED_PIXELS]);

#endif /* LE_MUSIC_RENDER_H */
