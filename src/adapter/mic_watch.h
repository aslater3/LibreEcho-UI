#ifndef LIBREECHO_MIC_WATCH_H
#define LIBREECHO_MIC_WATCH_H

#include <stdint.h>

#define LE_MIC_WATCH_LANES 7

/*
 * Per-lane level watch for the microphone array.
 *
 * The stock Amazon front end carries a dedicated "MicOcclusionDetector"
 * module and reports AFEDiagMicCompensationMaxGain/NumberofMics in its
 * diagnostics: it watches what each microphone actually hears and notices
 * when one of them is no longer contributing.  LibreEcho applied the static
 * IDME gains and then trusted all seven lanes forever, so a covered or
 * failed microphone silently degrades every mix that includes it.
 *
 * This detector is deliberately conservative.  It compares each lane with
 * the median of the lanes in use, over one-second windows, and only calls a
 * lane occluded after three consecutive windows more than 12 dB down.  A
 * lane has to recover for two windows before it is trusted again, and a
 * majority of lanes being "down" is treated as a change in level rather than
 * seven broken microphones.
 */
struct le_mic_watch {
    uint64_t energy[LE_MIC_WATCH_LANES];
    unsigned int frames;
    unsigned int miss_windows[LE_MIC_WATCH_LANES];
    unsigned int ok_windows[LE_MIC_WATCH_LANES];
    unsigned int occluded;
    unsigned long windows;
    int level_db[LE_MIC_WATCH_LANES];
};

void le_mic_watch_init(struct le_mic_watch *watch);

/*
 * Accumulates one sample across every lane.  Every full window the watch
 * re-evaluates the lanes named by "candidates" and returns the lanes it now
 * considers occluded as a bit mask.  A lane outside "candidates" is neither
 * watched nor reported.
 */
unsigned int le_mic_watch_feed(struct le_mic_watch *watch,
                               const int16_t logical[LE_MIC_WATCH_LANES],
                               unsigned int candidates);

/* Lanes currently considered occluded, as a bit mask. */
unsigned int le_mic_watch_occluded(const struct le_mic_watch *watch);

/*
 * Most recent level of each lane in dB relative to the loudest lane in the
 * window, or 0 for a lane that was not evaluated.  Only meaningful after at
 * least one full window.
 */
int le_mic_watch_level_db(const struct le_mic_watch *watch, int lane);

/* Number of complete one-second windows evaluated so far. */
unsigned long le_mic_watch_windows(const struct le_mic_watch *watch);

#endif
