/*
 * Common LED hardware-output stage.
 *
 * Every steady colour, notification pattern, music scene, transition and test
 * frame funnels through led_output_process() before it reaches the sysfs
 * `frame` interface.  The stage applies, in order:
 *
 *   logical 12 x RGB frame
 *       -> user/profile brightness cap            (logical space)
 *       -> colour / white channel calibration     (logical space)
 *       -> perceptual transfer (gamma > 1)        (deterministic, monotonic)
 *       -> aggregate weighted channel-load budget (proportional whole frame)
 *       -> optional frame-to-frame load slew limit
 *       -> IS31/sysfs hardware write
 *
 * Brightness is applied in logical space before the transfer so that a fade
 * passes through the same calibrated curve as a steady colour.  The transfer
 * is applied exactly once; callers must not pre-scale channels, because the
 * sysfs/multicolour fallback is driven from the same corrected pixels.
 *
 * The transfer is non-boosting (see led_output_gamma), so the logical
 * brightness and the night/sleep caps remain true upper bounds on whatever
 * reaches the hardware -- a dim request cannot be made brighter by the curve.
 *
 * The built-in calibration constants (gains, weights, accents, frame budget)
 * are conservative *uncharacterised* software placeholders, not per-unit
 * measurements and not a hardware safety claim.  Physical current, thermal and
 * audio-interaction characterisation is a separate release gate; nothing here
 * may be described as calibrated or verified.
 */
#ifndef LE_LED_OUTPUT_H
#define LE_LED_OUTPUT_H

#include <stddef.h>

#define LE_LED_PIXELS   12
#define LE_LED_CHANNELS (LE_LED_PIXELS * 3)

/* A logical or rendered RGB pixel.  Components are 0..255. */
struct led_rgb {
    unsigned int r;
    unsigned int g;
    unsigned int b;
};

/*
 * Perceptual transfer for a PWM-driven ring: the physical brightness of an
 * LED follows a power law, so the drive value is raised to gamma 2 (the
 * integer curve ceil(value^2 / 255)).  The curve is:
 *   - zero-preserving (0 -> 0, so exact off stays exactly off);
 *   - monotonic non-decreasing (limits and ordering are preserved);
 *   - non-boosting (out <= in, so a logical brightness or night/sleep cap is a
 *     true ceiling on the output and a dim request cannot be brightened);
 *   - non-vanishing (a nonzero request keeps at least one count).
 * It is deterministic and integer-only, so host and device agree exactly.
 */
unsigned int led_output_gamma(unsigned int value);
/* Fill a 256-entry table with led_output_gamma() for offline parity checks. */
void led_output_gamma_table(unsigned char table[256]);

enum led_accent {
    LE_ACCENT_WARM_WHITE = 0,
    LE_ACCENT_COOL_WHITE
};

struct led_output_calibration {
    /* Relative per-channel output gain, 0..255 with 255 == unity. */
    unsigned int gain_r;
    unsigned int gain_g;
    unsigned int gain_b;
    /* Restrained named near-white points; deliberately not 255,255,255. */
    struct led_rgb warm_white;
    struct led_rgb cool_white;
    /* Relative per-channel power weights used to estimate frame load. */
    unsigned int weight_r;
    unsigned int weight_g;
    unsigned int weight_b;
    /* Reviewed aggregate frame budget in weighted load units. */
    unsigned int max_frame_load;
    /* Optional per-frame increase in aggregate load; 0 disables the limit. */
    unsigned int max_slew_per_frame;
};

const struct led_output_calibration *led_output_default_calibration(void);
/* True if the requested named accent exists and wrote *out. */
int led_output_accent(enum led_accent which, struct led_rgb *out);

struct led_output_diag {
    unsigned int effective_brightness; /* 0..255 value the cap produced */
    unsigned int raw_load;             /* weighted load before the limiter */
    unsigned int frame_load;           /* weighted load after the limiter */
    unsigned int max_load;             /* active budget */
    int limited;                       /* proportional budget scaling applied */
    int slew_limited;                  /* slew scaling applied */
};

/* Caller-owned state; only the previous accepted load is retained. */
struct led_output_state {
    unsigned int previous_load;
    int have_previous;
};

/*
 * Process one logical frame.  brightness is 0..100; the frame is scaled by it
 * in logical space.  out receives the corrected, budgeted pixels.  diag may be
 * NULL.  Returns 0 always; hardware submission is the caller's job.
 */
void led_output_process(const struct led_output_calibration *cal,
                        struct led_output_state *state,
                        const struct led_rgb logical[LE_LED_PIXELS],
                        unsigned int brightness,
                        struct led_rgb out[LE_LED_PIXELS],
                        struct led_output_diag *diag);

/* Convenience: one logical colour replicated across the whole ring. */
void led_output_process_solid(const struct led_output_calibration *cal,
                              struct led_output_state *state,
                              struct led_rgb logical, unsigned int brightness,
                              struct led_rgb out[LE_LED_PIXELS],
                              struct led_output_diag *diag);

/* Weighted load of an already-rendered frame under cal. */
unsigned int led_output_frame_load(const struct led_output_calibration *cal,
                                   const struct led_rgb pixels[LE_LED_PIXELS]);

#endif /* LE_LED_OUTPUT_H */
