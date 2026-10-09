#include "led_output.h"

/*
 * Conservative uncharacterised software limits.
 *
 * Nothing here is a per-unit measurement or a hardware safety claim.  These
 * are placeholder ceilings chosen to keep an unmeasured board inside a
 * plausible envelope: no ordinary user setting can raise them (brightness is
 * applied before the budget, so a full-brightness frame is still limited).
 * Physical current/thermal/audio-interaction characterisation is a separate
 * release gate; until that is done the budget is a defensive bound, not a
 * verified one.
 */
static const struct led_output_calibration le_default_calibration = {
    .gain_r = 255,
    .gain_g = 255,
    .gain_b = 255,
    /* Restrained near-white points: a warm and a cool tint, not 255,255,255. */
    .warm_white = {255, 214, 170},
    .cool_white = {214, 232, 255},
    /* Equal per-channel current weighting until a board profile is measured. */
    .weight_r = 100,
    .weight_g = 100,
    .weight_b = 100,
    /* Placeholder cap: ~65% of a theoretical full-white 36-channel frame
       (9180 units).  Unverified against real hardware; a defensive bound. */
    .max_frame_load = 5967,
    /* Frame-to-frame slew limiting is off until hardware testing justifies it. */
    .max_slew_per_frame = 0
};

const struct led_output_calibration *led_output_default_calibration(void)
{
    return &le_default_calibration;
}

unsigned int led_output_gamma(unsigned int value)
{
    if (value == 0U)
        return 0U;                    /* exact off is preserved */
    if (value >= 255U)
        return 255U;
    /*
     * ceil(value^2 / 255): the physical PWM perceptual transfer (gamma 2).
     * A linear duty is not a linear perceived level, so the drive value is
     * squared; ceil() keeps a small nonzero request from rounding to black.
     * Non-boosting: the result never exceeds the input, so a logical
     * brightness or night/sleep cap stays a true ceiling on the output.
     */
    return (value * value + 254U) / 255U;
}

void led_output_gamma_table(unsigned char table[256])
{
    unsigned int i;

    if (table == NULL)
        return;
    for (i = 0U; i < 256U; i++)
        table[i] = (unsigned char)led_output_gamma(i);
}

int led_output_accent(enum led_accent which, struct led_rgb *out)
{
    const struct led_output_calibration *cal = led_output_default_calibration();

    if (out == NULL)
        return 0;
    if (which == LE_ACCENT_WARM_WHITE) {
        *out = cal->warm_white;
        return 1;
    }
    if (which == LE_ACCENT_COOL_WHITE) {
        *out = cal->cool_white;
        return 1;
    }
    return 0;
}

static unsigned int le_scale_brightness(unsigned int channel,
                                        unsigned int brightness)
{
    if (brightness >= 100U)
        return channel;
    return (channel * brightness + 50U) / 100U;
}

static unsigned int le_apply_gain(unsigned int channel, unsigned int gain)
{
    if (gain >= 255U)
        return channel;
    return (channel * gain + 127U) / 255U;
}

unsigned int led_output_frame_load(const struct led_output_calibration *cal,
                                   const struct led_rgb pixels[LE_LED_PIXELS])
{
    unsigned long long load = 0;
    size_t i;

    if (cal == NULL)
        cal = &le_default_calibration;
    for (i = 0U; i < LE_LED_PIXELS; i++) {
        load += (unsigned long long)pixels[i].r * cal->weight_r;
        load += (unsigned long long)pixels[i].g * cal->weight_g;
        load += (unsigned long long)pixels[i].b * cal->weight_b;
    }
    load /= 100ULL;
    if (load > 0xFFFFFFFFULL)
        load = 0xFFFFFFFFULL;
    return (unsigned int)load;
}

void led_output_process(const struct led_output_calibration *cal,
                        struct led_output_state *state,
                        const struct led_rgb logical[LE_LED_PIXELS],
                        unsigned int brightness,
                        struct led_rgb out[LE_LED_PIXELS],
                        struct led_output_diag *diag)
{
    led_output_process_transfer(cal, state, logical, brightness,
                                LE_OUTPUT_TRANSFER_GAMMA, out, diag);
}

void led_output_process_transfer(const struct led_output_calibration *cal,
                                 struct led_output_state *state,
                                 const struct led_rgb logical[LE_LED_PIXELS],
                                 unsigned int brightness,
                                 enum led_output_transfer transfer,
                                 struct led_rgb out[LE_LED_PIXELS],
                                 struct led_output_diag *diag)
{
    struct led_output_diag local;
    unsigned int raw_load;
    size_t i;

    if (cal == NULL)
        cal = &le_default_calibration;
    if (brightness > 100U)
        brightness = 100U;
    if (diag == NULL)
        diag = &local;
    diag->effective_brightness = (brightness * 255U + 50U) / 100U;
    diag->limited = 0;
    diag->slew_limited = 0;
    diag->max_load = cal->max_frame_load;

    for (i = 0U; i < LE_LED_PIXELS; i++) {
        unsigned int r = logical[i].r;
        unsigned int g = logical[i].g;
        unsigned int b = logical[i].b;

        if (r > 255U) r = 255U;
        if (g > 255U) g = 255U;
        if (b > 255U) b = 255U;

        /* 1. brightness cap in logical space */
        r = le_scale_brightness(r, brightness);
        g = le_scale_brightness(g, brightness);
        b = le_scale_brightness(b, brightness);

        /* 2. channel calibration in logical space */
        r = le_apply_gain(r, cal->gain_r);
        g = le_apply_gain(g, cal->gain_g);
        b = le_apply_gain(b, cal->gain_b);

        /* 3. perceptual transfer, applied once */
        if (transfer == LE_OUTPUT_TRANSFER_LINEAR) {
            out[i].r = r;
            out[i].g = g;
            out[i].b = b;
        } else {
            out[i].r = led_output_gamma(r);
            out[i].g = led_output_gamma(g);
            out[i].b = led_output_gamma(b);
        }
    }

    /* 4. weighted whole-frame budget */
    raw_load = led_output_frame_load(cal, out);
    diag->raw_load = raw_load;
    if (cal->max_frame_load > 0U && raw_load > cal->max_frame_load) {
        unsigned int budget = cal->max_frame_load;

        for (i = 0U; i < LE_LED_PIXELS; i++) {
            out[i].r = out[i].r * budget / raw_load;
            out[i].g = out[i].g * budget / raw_load;
            out[i].b = out[i].b * budget / raw_load;
        }
        diag->limited = 1;
    }

    /* 5. optional frame-to-frame slew limit on load increases */
    if (state != NULL && cal->max_slew_per_frame > 0U) {
        unsigned int load = led_output_frame_load(cal, out);

        if (state->have_previous &&
            load > state->previous_load + cal->max_slew_per_frame) {
            unsigned int ceiling = state->previous_load +
                                   cal->max_slew_per_frame;

            for (i = 0U; i < LE_LED_PIXELS; i++) {
                out[i].r = out[i].r * ceiling / load;
                out[i].g = out[i].g * ceiling / load;
                out[i].b = out[i].b * ceiling / load;
            }
            diag->slew_limited = 1;
        }
    }

    diag->frame_load = led_output_frame_load(cal, out);
    if (state != NULL) {
        state->previous_load = diag->frame_load;
        state->have_previous = 1;
    }
}

void led_output_process_solid(const struct led_output_calibration *cal,
                              struct led_output_state *state,
                              struct led_rgb logical, unsigned int brightness,
                              struct led_rgb out[LE_LED_PIXELS],
                              struct led_output_diag *diag)
{
    struct led_rgb frame[LE_LED_PIXELS];
    size_t i;

    for (i = 0U; i < LE_LED_PIXELS; i++)
        frame[i] = logical;
    led_output_process(cal, state, frame, brightness, out, diag);
}
