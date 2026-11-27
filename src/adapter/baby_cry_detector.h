#ifndef LE_BABY_CRY_DETECTOR_H
#define LE_BABY_CRY_DETECTOR_H

#include <stddef.h>
#include <stdint.h>

/*
 * Baby-cry detector for the waked mono stream (16 kHz, S16_LE, 10 ms
 * frames).
 *
 * The stock device offloads sound events to Pryon's acoustic event pipeline
 * (libpryon.so builds an acoustic_event_detection_pipeline and reports
 * acoustic_event scores); LibreEcho has no such model, so this is a
 * deterministic periodicity detector rather than a classifier.
 *
 * An infant cry is a high-pitched, strongly voiced bout: a fundamental
 * between roughly 250 and 700 Hz with harmonics, held for several hundred
 * milliseconds, and repeated. Adult speech usually sits below that range
 * and does not hold one pitch that long without pauses. The detector
 * therefore requires a voiced run of at least 450 ms and a second bout
 * inside four seconds before it reports.
 *
 * Off by default. While off, process() does nothing and costs nothing. While
 * on, every frame pays for an energy gate; only frames that clear it pay for
 * the autocorrelation.
 *
 * The thresholds are tuned constants, not a measured standard. The unit
 * tests pin the decision rules on synthetic signals; a real infant on a real
 * microphone is still owed.
 */

#define LE_CRY_RATE 16000U
#define LE_CRY_FRAME_SAMPLES 160U
#define LE_CRY_MIN_F0_HZ 250.0f
#define LE_CRY_MAX_F0_HZ 700.0f
/* 700 Hz -> 22.9 samples, 250 Hz -> 64 samples. */
#define LE_CRY_MIN_LAG 22U
#define LE_CRY_MAX_LAG 64U
#define LE_CRY_MIN_VOICED_FRAMES 45U      /* 450 ms of one bout. */
#define LE_CRY_QUIET_FRAMES 25U           /* 250 ms ends a bout. */
#define LE_CRY_BOUT_WINDOW_FRAMES 400U    /* second bout within 4 s. */
#define LE_CRY_REFRACTORY_FRAMES 300U

struct le_baby_cry_event {
    uint64_t detection_sample;
    unsigned int bouts;
};

struct le_baby_cry_detector {
    int enabled;
    uint64_t frames;
    float noise_energy;
    unsigned int voiced_frames;
    unsigned int voiced_gap;
    unsigned int quiet_frames;
    int bout_counted;
    unsigned int bouts;
    uint64_t first_bout_end;
    unsigned int refractory_frames_left;
    uint64_t accepted_count;
};

void le_baby_cry_detector_init(struct le_baby_cry_detector *detector);

/*
 * Turn detection on or off. Changing the state clears any bout in progress,
 * so a toggle never reports an old cry.
 */
void le_baby_cry_detector_set_enabled(
    struct le_baby_cry_detector *detector, int enabled);

/*
 * Screen one 10 ms frame of LE_CRY_FRAME_SAMPLES samples. Returns 1 and
 * fills event when a cry is reported for this frame, 0 otherwise.
 */
int le_baby_cry_detector_process(
    struct le_baby_cry_detector *detector,
    const int16_t *samples,
    struct le_baby_cry_event *event);

#endif /* LE_BABY_CRY_DETECTOR_H */
