#ifndef LE_FALSE_WAKE_GUARD_H
#define LE_FALSE_WAKE_GUARD_H

#include <stddef.h>
#include <stdint.h>

/*
 * False wake prevention for libreecho-waked, modelled on the stock ASP
 * FalseWWPrevention module. The stock names its inputs directly
 * (rawMicClipped, afeOutClipped, fastVarianceThres, slowVarianceThres,
 * Active WW Variance Penalty): a detection that arrives while the capture
 * chain is clipped, wildly non-stationary or digitally silent is not trusted.
 *
 * LibreEcho has no AED/wake feature stream to score here, so this is a gate in
 * front of the arbitration and subscriber fan-out rather than a score
 * penalty. It is deliberately narrow: only severe overload, a transient far
 * outside the recent floor, or silence are refused, so a loud but genuine
 * wake word still gets through.
 *
 * The thresholds are tuned constants, not a measured standard. Unit tests pin
 * the rules on synthetic frames; a loud room on real hardware is still owed.
 */

#define LE_FW_RATE 16000U
#define LE_FW_FRAME_SAMPLES 160U
/* 800 ms of per-frame raw clipping counts. */
#define LE_FW_HISTORY_FRAMES 80U
#define LE_FW_WINDOW_FRAMES 100U
#define LE_FW_CLIP_SAMPLE 32700
/* Share of clipped samples in the window that counts as overload. */
#define LE_FW_CLIP_RATIO 0.02f
/* max/min frame RMS above this is a transient, not speech. */
#define LE_FW_VARIANCE_RATIO 50.0f
/* RMS below this (16-bit counts) is silence. */
#define LE_FW_QUIET_RMS 4.0f
/* A detection at or above this score survives the variance rule. */
#define LE_FW_STRONG_SCORE 0.90f

struct le_false_wake_result {
    float raw_clip_ratio;
    float clean_clip_ratio;
    float rms;
    float variance_ratio;
    int clipped;
    int wild;
    int quiet;
    int reject;
};

struct le_false_wake_guard {
    int enabled;
    unsigned int history[LE_FW_HISTORY_FRAMES];
    unsigned int history_position;
    unsigned int history_count;
};

void le_false_wake_guard_init(struct le_false_wake_guard *guard);
void le_false_wake_guard_set_enabled(struct le_false_wake_guard *guard,
                                     int enabled);

/* Record raw (pre-AEC) frame clipping. Called once per capture frame. */
void le_false_wake_guard_observe(struct le_false_wake_guard *guard,
                                 const int16_t *raw, size_t count);

/*
 * Judge a detection against the recent raw clipping history and the window of
 * cleaned audio before it. Returns 1 when the event should be refused, 0 when
 * it may proceed; result is filled either way.
 */
int le_false_wake_guard_evaluate(struct le_false_wake_guard *guard,
                                 const int16_t *clean, size_t count,
                                 float score,
                                 struct le_false_wake_result *result);

#endif /* LE_FALSE_WAKE_GUARD_H */
