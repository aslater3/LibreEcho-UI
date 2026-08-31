#ifndef LIBREECHO_WAKE_ACCEPT_H
#define LIBREECHO_WAKE_ACCEPT_H

/*
 * Whether a scored frame should fire a wake event. Split out of wake_worker.c
 * so the rule can be tested without an inference engine or a device.
 */

/* Supporting frames normally required before a detection is believed. */
#define LE_WAKE_SUPPORT_REQUIRED 2U

/*
 * ...and while the device is playing something. Measured on hardware, the wake
 * score peaks above threshold for a single frame over music and collapses on
 * the next, so requiring two makes the device impossible to interrupt. A false
 * wake during playback costs far less than being unable to stop it.
 */
#define LE_WAKE_SUPPORT_REQUIRED_PLAYING 1U

/*
 * `support` is how many of the recent frames cleared the support threshold.
 * Returns non-zero when this frame should raise a wake event; the caller still
 * owns the lockout that stops one utterance firing twice.
 */
int le_wake_accept(float score, float accept_threshold, unsigned int support,
                   int vad_active, int playback_active);

#endif /* LIBREECHO_WAKE_ACCEPT_H */
