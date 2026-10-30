#include "mic_array_render.h"

#include <math.h>
#include <string.h>

#include "mic_array_geometry.h"

#define RENDER_PI 3.14159265358979323846
#define RENDER_RATE 16000.0

void le_array_render_init(struct le_array_render *render, double azimuth_deg)
{
    double c = LE_MIC_ARRAY_SPEED_OF_SOUND_MM_S * 1e-3;
    double radius = LE_MIC_ARRAY_RING_RADIUS_UM * 1e-6;
    double ux = cos(azimuth_deg * RENDER_PI / 180.0);
    double uy = sin(azimuth_deg * RENDER_PI / 180.0);
    unsigned int m;
    int k;

    memset(render, 0, sizeof(*render));
    for (m = 0; m < LE_ARRAY_RENDER_MICS; ++m) {
        double x = 0.0, y = 0.0, delay, sum = 0.0;

        if (m != LE_MIC_ARRAY_CENTRE_LANE) {
            double a = (LE_MIC_ARRAY_LANE0_AZIMUTH_DEG +
                        m * LE_MIC_ARRAY_RING_STEP_DEG) * RENDER_PI / 180.0;
            x = radius * cos(a);
            y = radius * sin(a);
        }
        /* A microphone further toward the source hears it earlier. */
        delay = -(x * ux + y * uy) / c * RENDER_RATE;
        for (k = 0; k < LE_ARRAY_RENDER_TAPS; ++k) {
            /* taps[k] weights the sample k samples old; peak at HALF+delay. */
            double t = k - LE_ARRAY_RENDER_HALF - delay;
            double sinc = fabs(t) < 1e-9 ? 1.0
                                         : sin(RENDER_PI * t) / (RENDER_PI * t);
            double win = 0.5 + 0.5 * cos(RENDER_PI * t /
                                         (LE_ARRAY_RENDER_HALF + 1));
            render->taps[m][k] = (float)(sinc * win);
            sum += sinc * win;
        }
        for (k = 0; k < LE_ARRAY_RENDER_TAPS; ++k)
            render->taps[m][k] = (float)(render->taps[m][k] / sum);
    }
}

void le_array_render_push(struct le_array_render *render, float sample)
{
    render->history[render->pos] = sample;
    render->pos = (render->pos + 1) % LE_ARRAY_RENDER_HISTORY;
}

float le_array_render_lane(const struct le_array_render *render,
                           unsigned int mic)
{
    float acc = 0.0f;
    unsigned int k;

    for (k = 0; k < LE_ARRAY_RENDER_TAPS; ++k)
        acc += render->taps[mic][k] *
               render->history[(render->pos + LE_ARRAY_RENDER_HISTORY - 1 - k) %
                               LE_ARRAY_RENDER_HISTORY];
    return acc;
}
