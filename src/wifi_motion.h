#ifndef LE_WIFI_MOTION_H
#define LE_WIFI_MOTION_H
/*
 * Experimental Wi-Fi motion detector (#444).
 *
 * The radar-puffin Wi-Fi driver (mt-wifi, MT7668-class) exposes no per-frame
 * RSSI to the host and no channel state information (CSI). The only signal is
 * the associated AP link RSSI, which the firmware refreshes roughly every 5 s
 * and which is reported as whole dBm. This detector therefore works on a
 * coarse series: it keeps a bounded window of recent readings and reports
 * motion while the window spread (max - min, in dB) stays large.
 *
 * Integer-only, no allocation, no clock. The caller pushes one reading per
 * fresh RSSI value (not per poll). Off by default: nothing in the daemons
 * constructs or feeds a detector yet.
 */

#define LE_WIFI_MOTION_WINDOW 12
#define LE_WIFI_MOTION_MIN_SAMPLES 6
#define LE_WIFI_MOTION_ON_SPREAD 3
#define LE_WIFI_MOTION_OFF_SPREAD 1
#define LE_WIFI_MOTION_OFF_HOLD 4
#define LE_WIFI_MOTION_RSSI_MIN (-120)
#define LE_WIFI_MOTION_RSSI_MAX (-10)

struct le_wifi_motion {
    int samples[LE_WIFI_MOTION_WINDOW];
    int count;
    int head;
    int quiet;
    int active;
};

void le_wifi_motion_init(struct le_wifi_motion *motion);
/* Feeds one reading in dBm. Out-of-range readings (including the -1 "unknown"
 * sentinel used by networkd) are ignored. Returns 1 while motion is active. */
int le_wifi_motion_push(struct le_wifi_motion *motion, int rssi_dbm);

#endif
