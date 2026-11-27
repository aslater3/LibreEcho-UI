#include "false_wake_guard.h"

#include <math.h>
#include <string.h>

void le_false_wake_guard_init(struct le_false_wake_guard *guard)
{
    memset(guard, 0, sizeof(*guard));
}

void le_false_wake_guard_set_enabled(struct le_false_wake_guard *guard,
                                     int enabled)
{
    le_false_wake_guard_init(guard);
    guard->enabled = enabled ? 1 : 0;
}

void le_false_wake_guard_observe(struct le_false_wake_guard *guard,
                                 const int16_t *raw, size_t count)
{
    unsigned int clipped = 0;
    size_t i;

    if (!guard->enabled || !raw)
        return;
    for (i = 0; i < count; ++i) {
        int value = raw[i];

        if (value >= LE_FW_CLIP_SAMPLE || value <= -LE_FW_CLIP_SAMPLE)
            ++clipped;
    }
    guard->history[guard->history_position] = clipped;
    if (++guard->history_position == LE_FW_HISTORY_FRAMES)
        guard->history_position = 0;
    if (guard->history_count < LE_FW_HISTORY_FRAMES)
        ++guard->history_count;
}

/* Frame RMS, and the min/max over frames that carry any signal. */
static float window_statistics(const int16_t *samples, size_t count,
                               float *max_rms, float *min_rms,
                               float *clip_ratio)
{
    size_t frames = count / LE_FW_FRAME_SAMPLES;
    size_t total = 0, clipped = 0;
    float sum = 0.0f;
    size_t f;

    *max_rms = 0.0f;
    *min_rms = 0.0f;
    *clip_ratio = 0.0f;
    for (f = 0; f < frames; ++f) {
        const int16_t *frame = samples + f * LE_FW_FRAME_SAMPLES;
        float energy = 0.0f;
        size_t n;

        for (n = 0; n < LE_FW_FRAME_SAMPLES; ++n) {
            int value = frame[n];

            if (value >= LE_FW_CLIP_SAMPLE || value <= -LE_FW_CLIP_SAMPLE)
                ++clipped;
            energy += (float)value * (float)value;
        }
        total += LE_FW_FRAME_SAMPLES;
        sum += energy;
        energy /= (float)LE_FW_FRAME_SAMPLES;
        if (energy > 0.0f) {
            float rms = sqrtf(energy);

            if (rms > *max_rms)
                *max_rms = rms;
            if (*min_rms == 0.0f || rms < *min_rms)
                *min_rms = rms;
        }
    }
    if (total == 0)
        return 0.0f;
    if (clipped)
        *clip_ratio = (float)clipped / (float)total;
    return sqrtf(sum / (float)total);
}

int le_false_wake_guard_evaluate(struct le_false_wake_guard *guard,
                                 const int16_t *clean, size_t count,
                                 float score,
                                 struct le_false_wake_result *result)
{
    unsigned int clipped_raw = 0, frames, i;
    float max_rms = 0.0f, min_rms = 0.0f;

    memset(result, 0, sizeof(*result));
    if (!guard->enabled || !clean || count == 0)
        return 0;

    frames = guard->history_count;
    for (i = 0; i < frames; ++i)
        clipped_raw += guard->history[i];

    result->rms = window_statistics(clean, count, &max_rms, &min_rms,
                                    &result->clean_clip_ratio);
    result->raw_clip_ratio = frames
        ? (float)clipped_raw / (float)(frames * LE_FW_FRAME_SAMPLES)
        : 0.0f;
    if (min_rms > 0.0f)
        result->variance_ratio = max_rms / min_rms;
    else
        result->variance_ratio = max_rms > 0.0f ? LE_FW_VARIANCE_RATIO : 0.0f;

    /*
     * Overload is judged on both sides of the canceller, as the stock metrics
     * separate rawMicClipped from afeOutClipped. A clipped recogniser input is
     * not evidence for a wake word, whatever the model scored.
     */
    result->clipped = result->raw_clip_ratio > LE_FW_CLIP_RATIO ||
                      result->clean_clip_ratio > LE_FW_CLIP_RATIO;
    /*
     * A transient far outside the rest of the window (a door, a hand on the
     * device) can look like a wake word to a small model. A strong score still
     * wins, so a genuine shout is not thrown away.
     */
    result->wild = result->variance_ratio > LE_FW_VARIANCE_RATIO &&
                   score < LE_FW_STRONG_SCORE;
    result->quiet = result->rms < LE_FW_QUIET_RMS;
    result->reject = result->clipped || result->wild || result->quiet;
    return result->reject;
}
