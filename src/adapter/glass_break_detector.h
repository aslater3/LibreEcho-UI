#ifndef LE_GLASS_BREAK_DETECTOR_H
#define LE_GLASS_BREAK_DETECTOR_H

#include <stddef.h>
#include <stdint.h>

/*
 * Glass-break detector for the waked mono stream (16 kHz, S16_LE, 10 ms
 * frames).
 *
 * The stock device offloads sound events to Pryon's acoustic event pipeline
 * (libpryon.so builds an acoustic_event_detection_pipeline and reports
 * acoustic_event scores); LibreEcho has no such model, so this is a
 * deterministic spectral detector rather than a classifier.
 *
 * A pane breaking is a sharp, broadband, high-frequency transient (the
 * shatter) followed within a few hundred milliseconds by smaller high
 * fragments (the tinkle). A door slam, a clap or speech may produce the
 * first burst but not the high-frequency pair, so the detector requires two
 * high-band bursts inside the tinkle window.
 *
 * Off by default. While off, process() does nothing and costs nothing. While
 * on, every frame pays for a cheap energy gate; only frames near a burst pay
 * for the eight Goertzel filters.
 *
 * The thresholds are tuned constants, not a measured standard. The unit
 * tests pin the decision rules on synthetic signals; a real pane of glass on
 * a real microphone is still owed.
 */

#define LE_GLASS_RATE 16000U
#define LE_GLASS_FRAME_SAMPLES 160U
#define LE_GLASS_HIGH_BANDS 5U
#define LE_GLASS_LOW_BANDS 2U
#define LE_GLASS_TINKLE_FRAMES 70U   /* 700 ms between the shatter and tinkle. */
#define LE_GLASS_REFRACTORY_FRAMES 100U

struct le_glass_break_event {
    uint64_t detection_sample;
    unsigned int bursts;
};

struct le_glass_break_detector {
    int enabled;
    uint64_t frames;
    float noise_energy;
    int in_burst;
    unsigned int burst_frames;
    uint64_t burst_start_frame;
    unsigned int bursts;
    uint64_t first_burst_frame;
    unsigned int refractory_frames_left;
    uint64_t accepted_count;
};

void le_glass_break_detector_init(struct le_glass_break_detector *detector);

/*
 * Turn detection on or off. Changing the state clears any burst in progress,
 * so a toggle never reports an old shatter.
 */
void le_glass_break_detector_set_enabled(
    struct le_glass_break_detector *detector, int enabled);

/*
 * Screen one 10 ms frame of LE_GLASS_FRAME_SAMPLES samples. Returns 1 and
 * fills event when a glass break is reported for this frame, 0 otherwise.
 */
int le_glass_break_detector_process(
    struct le_glass_break_detector *detector,
    const int16_t *samples,
    struct le_glass_break_event *event);

#endif /* LE_GLASS_BREAK_DETECTOR_H */
