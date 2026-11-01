/* Experimental Wi-Fi motion detector (#444): coarse RSSI series, hysteresis, rejects. */
#include "wifi_motion.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } } while (0)

static int feed(struct le_wifi_motion *m, const int *series, size_t n)
{
    int last = 0;
    size_t i;

    for (i = 0; i < n; i++)
        last = le_wifi_motion_push(m, series[i]);
    return last;
}

int main(void)
{
    struct le_wifi_motion m;
    /* Radar capture, 10 s, quiet room, no deliberate motion (device-recorded
     * with le-device sh on radar, 2026-11-01): the wlan0 level read from
     * /proc/net/wireless changed -60 -> -59 -> -60 dBm, then held. Fed here as
     * one reading per fresh firmware value. */
    static const int quiet_recorded[] = { -60, -59, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60, -60 };
    /* SYNTHETIC walk-by: not a recording. Alternating swings of 6-10 dB on the
     * AP link, the coarse pattern a body moving through the path could cause. */
    static const int synthetic_walk[] = { -52, -58, -50, -57, -53, -59, -51, -58, -52, -57, -50, -58 };
    static const int still_after_walk[] = { -55, -55, -55, -55, -55, -55, -55, -55, -55, -55, -55, -55, -55, -55, -55, -55 };
    static const int invalid[] = { -1, 0, -130, 5, -9 };
    int state, i;

    le_wifi_motion_init(&m);
    state = feed(&m, quiet_recorded, sizeof quiet_recorded / sizeof quiet_recorded[0]);
    CHECK(state == 0, "quiet recorded trace never reports motion");
    CHECK(m.active == 0, "quiet recorded trace leaves detector idle");

    le_wifi_motion_init(&m);
    for (i = 0; i < 5; i++)
        state = le_wifi_motion_push(&m, synthetic_walk[i]);
    CHECK(state == 0, "fewer than MIN_SAMPLES readings never report motion");
    CHECK(le_wifi_motion_push(&m, synthetic_walk[5]) == 1, "synthetic walk trips motion at MIN_SAMPLES readings");
    state = feed(&m, synthetic_walk + 6, sizeof synthetic_walk / sizeof synthetic_walk[0] - 6);
    CHECK(state == 1, "motion stays active while the swings persist");

    /* Flat readings: the spread-producing samples are still in the window for
     * several pushes, so motion must hold, then clear after the quiet hold. */
    for (i = 0; i < 6; i++)
        state = le_wifi_motion_push(&m, still_after_walk[i]);
    CHECK(state == 1, "motion holds while the window still contains swings");
    state = feed(&m, still_after_walk + 6, sizeof still_after_walk / sizeof still_after_walk[0] - 6);
    CHECK(state == 0, "motion clears after a flat window and the quiet hold");

    le_wifi_motion_init(&m);
    for (i = 0; i < (int)(sizeof invalid / sizeof invalid[0]); i++)
        state = le_wifi_motion_push(&m, invalid[i]);
    CHECK(m.count == 0, "sentinels and out-of-range readings do not enter the window");
    CHECK(state == 0, "invalid readings never trip motion");
    le_wifi_motion_push(&m, -55);
    CHECK(m.count == 1 && m.samples[0] == -55, "the first in-range reading is stored");

    le_wifi_motion_init(&m);
    CHECK(le_wifi_motion_push(NULL, -55) == 0, "NULL detector is inert");
    le_wifi_motion_init(NULL);

    if (failures) {
        fprintf(stderr, "test-wifi-motion: %d failure(s)\n", failures);
        return 1;
    }
    puts("test-wifi-motion: PASS");
    return 0;
}
