#ifndef LIBREECHO_MIC_ARRAY_GEOMETRY_H
#define LIBREECHO_MIC_ARRAY_GEOMETRY_H

/*
 * Physical layout of the Radar (Echo 2nd generation) seven-microphone array,
 * measured on G090P308901401LH by scratching each port in turn and recording
 * all nine capture lanes: every port lit exactly one lane, 23-30 dB above the
 * next loudest.
 *
 * Reference frame, looking down on the top of the device:
 *   0 degrees  = front, the volume-down button
 *   90 degrees = mute (left as seen from the front)
 *   180        = rear, the volume-up button
 *   270        = action (right as seen from the front)
 * Angles increase clockwise when viewed from above.
 *
 *   lane 0 rear-left    150      lane 3 front-right  330
 *   lane 1 rear-right   210      lane 4 front-left    30
 *   lane 2 action       270      lane 5 mute          90
 *   lane 6 centre
 *
 * Lanes 0-5 run round the ring in order, so lane n sits at 150 + 60n degrees
 * and the opposite pairs are 0/3, 1/4 and 2/5.  This agrees with the stock
 * beamformer coefficients, which treat 0/3 and 1/4 as diametric pairs, and
 * with the stock speech front end, which uses lanes 0, 1, 3 and 4.
 *
 * Timing: a chirp from the device's own speaker, referenced to the playback
 * loopback lane, reached all six ring lanes within +/-0.4 samples at 16 kHz
 * (lanes 0 and 3 differ by 0.12) and the centre lane about one sample later.
 * The capture path therefore adds no lane-to-lane offset; any inter-lane delay
 * in a recording is acoustic.
 */
#define LE_MIC_ARRAY_RING_LANES 6
#define LE_MIC_ARRAY_CENTRE_LANE 6
#define LE_MIC_ARRAY_LANE0_AZIMUTH_DEG 150
#define LE_MIC_ARRAY_RING_STEP_DEG 60
/*
 * Ring radius in micrometres.  Measured by hand on the same unit: 72 mm between
 * the mute and action port centres (lanes 5 and 2, a diameter) and 35.5 mm
 * from the mute port to the centre port.
 */
#define LE_MIC_ARRAY_RING_RADIUS_UM 36000
#define LE_MIC_ARRAY_SPEED_OF_SOUND_MM_S 343000

#endif
