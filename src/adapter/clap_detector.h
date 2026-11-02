#ifndef LE_CLAP_DETECTOR_H
#define LE_CLAP_DETECTOR_H

#include <stddef.h>
#include <stdint.h>

/*
 * Clap detector for the waked mono stream (16 kHz, S16_LE, 10 ms frames).
 *
 * Off by default. While off, process() does nothing and costs nothing. While
 * on, every frame is screened by a cheap energy test first, and only frames
 * that pass it pay for the spectral check, so silence and steady sound cost
 * almost nothing.
 *
 * A clap is a short broadband transient:
 *   - energy jumps well above a slowly tracked noise floor (onset),
 *   - the peak is impulsive (peak / rms above a crest floor),
 *   - the spectrum is broadband, measured as spectral flatness over eight
 *     Goertzel bands (speech and music are peakier),
 *   - and the energy falls back by a clear margin within a few frames
 *     (short decay). Speech and music sustain.
 *
 * Two claps inside the double window are reported as one double event; a
 * single clap is reported once the window has passed without a second clap,
 * so a single clap is reported slightly late by design.
 */

#define LE_CLAP_RATE 16000U
#define LE_CLAP_FRAME_SAMPLES 160U
#define LE_CLAP_BANDS 8U

#define LE_CLAP_SINGLE 1U
#define LE_CLAP_DOUBLE 2U

struct le_clap_event {
    uint64_t detection_sample;
    unsigned int claps;
};

struct le_clap_detector {
    int enabled;
    uint64_t frames;
    float noise_energy;
    int watching;
    float watch_peak_energy;
    uint64_t watch_peak_frame;
    unsigned int refractory_frames_left;
    int pending_single;
    uint64_t pending_frame;
    uint64_t accepted_count;
};

void le_clap_detector_init(struct le_clap_detector *detector);

/*
 * Turn detection on or off. Changing the state clears any clap in progress and
 * any pending single, so a toggle never reports an old clap.
 */
void le_clap_detector_set_enabled(struct le_clap_detector *detector,
                                  int enabled);

/*
 * Screen one 10 ms frame of LE_CLAP_FRAME_SAMPLES samples. Returns 1 and fills
 * event when a single or double clap is reported for this frame, 0 otherwise.
 * At most one event is reported per frame.
 */
int le_clap_detector_process(struct le_clap_detector *detector,
                             const int16_t *samples,
                             struct le_clap_event *event);

#endif /* LE_CLAP_DETECTOR_H */
