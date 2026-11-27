#include "adapter/mic_watch.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

#define ALL_LANES 0x7fU
#define WINDOW 16000U

/*
 * A steady tone on every lane, with "down_lane" attenuated by "permille"
 * (1000 = unchanged).  One sample per call is what the daemon does.
 */
static void feed_windows(struct le_mic_watch *watch, unsigned int candidates,
                         int amplitude, int down_lane, int down_permille,
                         unsigned int windows)
{
    unsigned int n;
    unsigned int frame;

    for (n = 0; n < windows * WINDOW; ++n) {
        int16_t logical[LE_MIC_WATCH_LANES];
        int lane;

        for (lane = 0; lane < LE_MIC_WATCH_LANES; ++lane) {
            long value = ((n & 1U) ? amplitude : -amplitude);
            if (lane == down_lane)
                value = value * down_permille / 1000;
            logical[lane] = (int16_t)value;
        }
        frame = le_mic_watch_feed(watch, logical, candidates);
        (void)frame;
    }
}

int main(void)
{
    struct le_mic_watch watch;

    /* A quiet room never trips the watch, however long it runs. */
    le_mic_watch_init(&watch);
    feed_windows(&watch, ALL_LANES, 10, -1, 1000, 10);
    CHECK(le_mic_watch_occluded(&watch) == 0);

    /* Six healthy lanes at the same level stay healthy. */
    le_mic_watch_init(&watch);
    feed_windows(&watch, ALL_LANES, 800, -1, 1000, 6);
    CHECK(le_mic_watch_occluded(&watch) == 0);
    CHECK(le_mic_watch_level_db(&watch, 3) == 0);

    /*
     * Lane 4 is 16 dB down: one window is not enough, three confirm it.
     * This mirrors the stock front end's MicOcclusionDetector, which the
     * fixed-gain LibreEcho path had no equivalent for.
     */
    le_mic_watch_init(&watch);
    feed_windows(&watch, ALL_LANES, 800, 4, 158, 2);
    CHECK(le_mic_watch_occluded(&watch) == 0);
    feed_windows(&watch, ALL_LANES, 800, 4, 158, 1);
    CHECK(le_mic_watch_occluded(&watch) == (1U << 4));
    CHECK(le_mic_watch_level_db(&watch, 4) <= -12);
    CHECK(le_mic_watch_level_db(&watch, 0) == 0);

    /* A lane that is only 6 dB down is quieter, not broken. */
    le_mic_watch_init(&watch);
    feed_windows(&watch, ALL_LANES, 800, 2, 501, 6);
    CHECK(le_mic_watch_occluded(&watch) == 0);

    /* Once it hears again for two windows the lane is trusted again. */
    feed_windows(&watch, ALL_LANES, 800, -1, 1000, 2);
    CHECK(le_mic_watch_occluded(&watch) == 0);

    /*
     * Three of seven lanes down is normal; four is a different problem and
     * the watch leaves the selection alone instead of chasing it.
     */
    le_mic_watch_init(&watch);
    {
        unsigned int n;

        for (n = 0; n < 6 * WINDOW; ++n) {
            int16_t logical[LE_MIC_WATCH_LANES];
            int lane;

            for (lane = 0; lane < LE_MIC_WATCH_LANES; ++lane)
                logical[lane] = (int16_t)((n & 1U) ? 800 : -800);
            for (lane = 0; lane < 4; ++lane)
                logical[lane] = (int16_t)((n & 1U) ? 100 : -100);
            (void)le_mic_watch_feed(&watch, logical, ALL_LANES);
        }
    }
    CHECK(le_mic_watch_occluded(&watch) == 0);

    /* Lanes that are not in the candidate mask are neither watched nor
       reported, so the "single microphone" mix never flags the others. */
    le_mic_watch_init(&watch);
    feed_windows(&watch, 1U << 0, 800, 1, 100, 6);
    CHECK(le_mic_watch_occluded(&watch) == 0);

    printf("mic_watch: occlusion watch and lane levels ok\n");
    return 0;
}
