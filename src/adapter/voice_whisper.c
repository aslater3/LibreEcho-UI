#define _POSIX_C_SOURCE 200809L

#include "voice_whisper.h"

/* Active-speech gate: frame RMS in int16 units. Quiet room noise sits well
   below this on the stock microphone path; speech sits far above it. */
#define ACTIVE_FRAME_MEAN_SQUARE_MIN (300LL * 300LL)
/* Pitch search: lags 40..200 samples are 400 Hz..80 Hz at 16 kHz. */
#define PITCH_LAG_MIN 40U
#define PITCH_LAG_MAX 200U
/* A frame is voiced when its best normalised autocorrelation ratio reaches
   0.35. The comparison is on the squared ratio, so no square root is needed. */
#define VOICED_RATIO_MIN_SQUARED_PERMILLE 122U
/* Whispers: at least 60% of active frames unvoiced, and the first-difference
   energy at least 40% of signal energy on average. */
#define WHISPER_UNVOICED_PERMILLE_MIN 600U
#define WHISPER_ROUGHNESS_PERMILLE_MIN 400U

static void analyse_frame(const int16_t *frame,
                          unsigned int *best_squared_permille,
                          unsigned int *roughness_permille)
{
    unsigned int n = LE_VOICE_WHISPER_FRAME_SAMPLES;
    unsigned int lag, i;
    long long energy = 0, difference = 0;
    double best_ratio_squared = 0.0;

    for (i = 0; i < n; ++i) {
        long long value = frame[i];

        energy += value * value;
        if (i) {
            long long step = value - frame[i - 1];

            difference += step * step;
        }
    }
    *roughness_permille = energy
        ? (unsigned int)((difference * 1000LL) / energy) : 0U;

    for (lag = PITCH_LAG_MIN; lag <= PITCH_LAG_MAX; ++lag) {
        long long cross = 0, overlap_energy = 0, lagged_energy = 0;

        for (i = lag; i < n; ++i) {
            long long a = frame[i];
            long long b = frame[i - lag];

            cross += a * b;
            overlap_energy += a * a;
            lagged_energy += b * b;
        }
        if (cross <= 0 || !lagged_energy || !overlap_energy)
            continue;
        {
            /* Normalised over the overlapping span, as r^2 / (E_a * E_b),
               computed without a square root. */
            double ratio = (double)cross * (double)cross /
                ((double)overlap_energy * (double)lagged_energy);

            if (ratio > best_ratio_squared)
                best_ratio_squared = ratio;
        }
    }
    *best_squared_permille = (unsigned int)(best_ratio_squared * 1000.0);
}

int le_voice_whisper_classify(const int16_t *samples, size_t count,
                              struct le_voice_whisper_result *result)
{
    size_t offset;
    unsigned long long harmonicity_sum = 0;
    unsigned long long roughness_sum = 0;
    unsigned int analysed = 0;

    if (!samples || !result)
        return -1;
    result->active_frames = 0;
    result->unvoiced_frames = 0;
    result->harmonicity_permille = 0;
    result->roughness_permille = 0;
    result->whispered = 0;

    for (offset = 0; offset + LE_VOICE_WHISPER_FRAME_SAMPLES <= count &&
             analysed < LE_VOICE_WHISPER_MAX_ANALYSED_FRAMES;
         offset += LE_VOICE_WHISPER_FRAME_SAMPLES) {
        const int16_t *frame = samples + offset;
        unsigned int best_squared, roughness;
        long long energy = 0;
        unsigned int i;

        for (i = 0; i < LE_VOICE_WHISPER_FRAME_SAMPLES; ++i)
            energy += (long long)frame[i] * frame[i];
        if (energy / LE_VOICE_WHISPER_FRAME_SAMPLES <
            ACTIVE_FRAME_MEAN_SQUARE_MIN)
            continue;
        analyse_frame(frame, &best_squared, &roughness);
        ++analysed;
        ++result->active_frames;
        if (best_squared < VOICED_RATIO_MIN_SQUARED_PERMILLE)
            ++result->unvoiced_frames;
        harmonicity_sum += best_squared;
        roughness_sum += roughness;
    }
    if (result->active_frames < LE_VOICE_WHISPER_MIN_ACTIVE_FRAMES)
        return -1;
    result->harmonicity_permille =
        (unsigned int)(harmonicity_sum / result->active_frames);
    result->roughness_permille =
        (unsigned int)(roughness_sum / result->active_frames);
    result->whispered =
        result->unvoiced_frames * 1000U >=
            result->active_frames * WHISPER_UNVOICED_PERMILLE_MIN &&
        result->roughness_permille >= WHISPER_ROUGHNESS_PERMILLE_MIN;
    return 0;
}
