#include "wifi_motion.h"

#include <string.h>

void le_wifi_motion_init(struct le_wifi_motion *motion)
{
    if (!motion)
        return;
    memset(motion, 0, sizeof(*motion));
}

static int window_spread(const struct le_wifi_motion *motion)
{
    int i, lo = 0, hi = 0;

    for (i = 0; i < motion->count; i++) {
        int value = motion->samples[i];

        if (i == 0 || value < lo)
            lo = value;
        if (i == 0 || value > hi)
            hi = value;
    }
    return hi - lo;
}

int le_wifi_motion_push(struct le_wifi_motion *motion, int rssi_dbm)
{
    int spread;

    if (!motion)
        return 0;
    if (rssi_dbm < LE_WIFI_MOTION_RSSI_MIN || rssi_dbm > LE_WIFI_MOTION_RSSI_MAX)
        return motion->active;

    motion->samples[motion->head] = rssi_dbm;
    motion->head = (motion->head + 1) % LE_WIFI_MOTION_WINDOW;
    if (motion->count < LE_WIFI_MOTION_WINDOW)
        motion->count++;

    if (motion->count < LE_WIFI_MOTION_MIN_SAMPLES)
        return motion->active;

    spread = window_spread(motion);
    if (spread >= LE_WIFI_MOTION_ON_SPREAD) {
        motion->active = 1;
        motion->quiet = 0;
    } else if (spread <= LE_WIFI_MOTION_OFF_SPREAD && motion->active) {
        motion->quiet++;
        if (motion->quiet >= LE_WIFI_MOTION_OFF_HOLD) {
            motion->active = 0;
            motion->quiet = 0;
        }
    } else {
        motion->quiet = 0;
    }
    return motion->active;
}
