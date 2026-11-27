#include "mic_watch.h"

#include <string.h>

#define LE_MIC_WATCH_RATE 16000
#define LE_MIC_WATCH_WINDOW_SAMPLES LE_MIC_WATCH_RATE

/*
 * 12 dB below the median lane, as an energy ratio.  A microphone that is
 * merely a little quieter than its neighbours is normal (the IDME gains only
 * equalise the codec path); 12 dB is the point where the lane stops adding
 * anything to a sum and starts dragging the beam directions with it.
 */
#define LE_MIC_WATCH_DROP_NUM 63U
#define LE_MIC_WATCH_DROP_DEN 1000U

/* A quiet room must not make every lane look equal; only compare when the
 * loudest lane is clearly above the 12-17 RMS idle floor. */
#define LE_MIC_WATCH_MIN_RMS 20U

#define LE_MIC_WATCH_CONFIRM_WINDOWS 3U
#define LE_MIC_WATCH_RECOVER_WINDOWS 2U

static uint64_t isqrt64(uint64_t value)
{
    uint64_t root = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > value)
        bit >>= 2;
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

static int lane_db(int level, int loudest)
{
    /*
     * 20*log10(level/loudest) is the amplitude ratio.  Integer log10 avoids
     * pulling a math header into the hot path for a value only ever printed.
     */
    int db;

    if (level <= 0 || loudest <= 0)
        return -60;
    db = 0;
    while (level * 2 < loudest) {
        level *= 2;
        db -= 6;
    }
    while (level > loudest) {
        level /= 2;
        db += 6;
    }
    if (db < -60)
        db = -60;
    return db;
}

void le_mic_watch_init(struct le_mic_watch *watch)
{
    memset(watch, 0, sizeof(*watch));
}

static uint64_t median_energy(const uint64_t *energy,
                              const unsigned int *lanes,
                              unsigned int count)
{
    uint64_t sorted[LE_MIC_WATCH_LANES];
    unsigned int i, j;

    for (i = 0; i < count; ++i) {
        uint64_t value = energy[lanes[i]];

        for (j = i; j > 0 && sorted[j - 1] > value; --j)
            sorted[j] = sorted[j - 1];
        sorted[j] = value;
    }
    if (count == 1)
        return sorted[0];
    if (count & 1U)
        return sorted[count / 2];
    return (sorted[count / 2 - 1] + sorted[count / 2]) / 2;
}

unsigned int le_mic_watch_feed(struct le_mic_watch *watch,
                               const int16_t logical[LE_MIC_WATCH_LANES],
                               unsigned int candidates)
{
    unsigned int lanes[LE_MIC_WATCH_LANES];
    unsigned int count = 0;
    unsigned int down = 0;
    uint64_t loudest = 0;
    uint64_t median;
    unsigned int i;

    if (!watch)
        return 0;
    for (i = 0; i < LE_MIC_WATCH_LANES; ++i) {
        int64_t sample = logical[i];

        watch->energy[i] += (uint64_t)(sample * sample);
    }
    if (++watch->frames < LE_MIC_WATCH_WINDOW_SAMPLES)
        return watch->occluded;

    for (i = 0; i < LE_MIC_WATCH_LANES; ++i) {
        if (!(candidates & (1U << i)))
            continue;
        lanes[count++] = i;
        if (watch->energy[i] > loudest)
            loudest = watch->energy[i];
    }
    for (i = 0; i < LE_MIC_WATCH_LANES; ++i)
        watch->level_db[i] = 0;
    ++watch->windows;
    for (i = 0; i < count; ++i) {
        int rms = (int)isqrt64(watch->energy[lanes[i]] /
                               LE_MIC_WATCH_WINDOW_SAMPLES);

        watch->level_db[lanes[i]] =
            lane_db(rms, (int)isqrt64(loudest / LE_MIC_WATCH_WINDOW_SAMPLES));
    }

    /* Nothing worth comparing: a quiet room, or a single lane in use. */
    if (count < 2 ||
        isqrt64(loudest / LE_MIC_WATCH_WINDOW_SAMPLES) < LE_MIC_WATCH_MIN_RMS) {
        for (i = 0; i < LE_MIC_WATCH_LANES; ++i)
            watch->energy[i] = 0;
        watch->frames = 0;
        return watch->occluded;
    }

    median = median_energy(watch->energy, lanes, count);
    for (i = 0; i < count; ++i) {
        unsigned int lane = lanes[i];

        if (watch->energy[lane] * LE_MIC_WATCH_DROP_DEN <
            median * LE_MIC_WATCH_DROP_NUM) {
            if (watch->miss_windows[lane] < 0xffffU)
                ++watch->miss_windows[lane];
            watch->ok_windows[lane] = 0;
        } else {
            watch->miss_windows[lane] = 0;
            if (watch->ok_windows[lane] < 0xffffU)
                ++watch->ok_windows[lane];
        }
    }

    for (i = 0; i < count; ++i) {
        unsigned int lane = lanes[i];

        if (watch->occluded & (1U << lane)) {
            if (watch->ok_windows[lane] >= LE_MIC_WATCH_RECOVER_WINDOWS)
                watch->occluded &= ~(1U << lane);
        } else if (watch->miss_windows[lane] >= LE_MIC_WATCH_CONFIRM_WINDOWS) {
            watch->occluded |= 1U << lane;
        }
    }

    for (i = 0; i < count; ++i)
        down += (watch->occluded >> lanes[i]) & 1U;
    /*
     * If half or more of the lanes in use look dead the array has changed
     * level, not lost microphones; leave the selection alone rather than
     * reducing it to one noisy lane.
     */
    if (down * 2 >= count) {
        watch->occluded &= ~candidates;
        for (i = 0; i < LE_MIC_WATCH_LANES; ++i) {
            watch->miss_windows[i] = 0;
            watch->ok_windows[i] = 0;
        }
    }
    for (i = 0; i < LE_MIC_WATCH_LANES; ++i)
        watch->energy[i] = 0;
    watch->frames = 0;
    return watch->occluded;
}

unsigned int le_mic_watch_occluded(const struct le_mic_watch *watch)
{
    return watch ? watch->occluded : 0;
}

int le_mic_watch_level_db(const struct le_mic_watch *watch, int lane)
{
    if (!watch || lane < 0 || lane >= LE_MIC_WATCH_LANES)
        return 0;
    return watch->level_db[lane];
}

unsigned long le_mic_watch_windows(const struct le_mic_watch *watch)
{
    return watch ? watch->windows : 0;
}
