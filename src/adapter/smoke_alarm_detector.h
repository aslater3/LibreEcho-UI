#ifndef LE_SMOKE_ALARM_DETECTOR_H
#define LE_SMOKE_ALARM_DETECTOR_H

#include <stddef.h>
#include <stdint.h>

/*
 * Smoke alarm detector for the waked mono stream (16 kHz, S16_LE, 10 ms
 * frames). It targets the T3 temporal pattern of a smoke alarm: a steady
 * tone near 3 kHz in pulses of about half a second, separated by gaps of
 * about half a second, repeated three times.
 *
 * Off by default. While off, process() does nothing and costs nothing. While
 * on, each frame gets an energy gate and then three Goertzel filters (2.9,
 * 3.1 and 3.3 kHz, 160 samples each). A frame is tonal when one filter holds
 * most of the frame energy at its frequency. Noise and speech are not
 * steady, so they do not form the on/off sequence.
 *
 * The timing windows and the tonal threshold are tuned constants, not a
 * measured standard. The unit tests pin the decision rules on synthetic
 * signals only. A real alarm on a real microphone is still owed.
 */

#define LE_SMOKE_RATE 16000U
#define LE_SMOKE_FRAME_SAMPLES 160U
#define LE_SMOKE_PULSES_REQUIRED 3U

struct le_smoke_alarm_event {
    uint64_t detection_sample;
    unsigned int pulses;
};

struct le_smoke_alarm_detector {
    int enabled;
    uint64_t frames;
    float noise_energy;
    int in_pulse;
    unsigned int tone_frames;
    unsigned int silent_frames;
    unsigned int pulses;
    uint64_t first_pulse_frame;
    unsigned int cooldown_frames_left;
};

void le_smoke_alarm_detector_init(struct le_smoke_alarm_detector *detector);

/*
 * Turn detection on or off. Changing the state clears any pulse sequence in
 * progress, so a toggle never reports an old alarm.
 */
void le_smoke_alarm_detector_set_enabled(
    struct le_smoke_alarm_detector *detector, int enabled);

/*
 * Screen one 10 ms frame of LE_SMOKE_FRAME_SAMPLES samples. Returns 1 and
 * fills event when a complete T3 sequence ends on this frame, 0 otherwise.
 */
int le_smoke_alarm_detector_process(struct le_smoke_alarm_detector *detector,
                                    const int16_t *samples,
                                    struct le_smoke_alarm_event *event);

#endif /* LE_SMOKE_ALARM_DETECTOR_H */
