#ifndef LIBREECHO_MIC_ARRAY_RENDER_H
#define LIBREECHO_MIC_ARRAY_RENDER_H

/*
 * Renders a mono signal onto the seven microphone lanes as a far-field plane
 * wave from a given azimuth, using the measured layout in
 * mic_array_geometry.h: each lane gets that microphone's fractional
 * propagation delay through a windowed-sinc interpolator.
 *
 * Used wherever synthetic audio is put on the array (audio injection in the
 * capture mux, the voice-e2e harness), so the beamformer sees what a real
 * talker would produce rather than identical lanes, which it would treat as
 * a source directly overhead.
 *
 * Output lags input by LE_ARRAY_RENDER_HALF samples.
 */
#define LE_ARRAY_RENDER_MICS 7
#define LE_ARRAY_RENDER_HALF 12
#define LE_ARRAY_RENDER_TAPS (2 * LE_ARRAY_RENDER_HALF + 1)
#define LE_ARRAY_RENDER_HISTORY 32

struct le_array_render {
    float history[LE_ARRAY_RENDER_HISTORY];
    unsigned int pos;
    float taps[LE_ARRAY_RENDER_MICS][LE_ARRAY_RENDER_TAPS];
};

/* azimuth_deg: 0 = front (volume down), clockwise viewed from above. */
void le_array_render_init(struct le_array_render *render, double azimuth_deg);
void le_array_render_push(struct le_array_render *render, float sample);
/* The most recently pushed sample, as microphone lane mic receives it. */
float le_array_render_lane(const struct le_array_render *render,
                           unsigned int mic);

#endif
