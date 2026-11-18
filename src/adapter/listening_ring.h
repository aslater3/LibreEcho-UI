#ifndef LIBREECHO_LISTENING_RING_H
#define LIBREECHO_LISTENING_RING_H

/*
 * Alexa-style listening ring: the whole ring in the Listening theme colour,
 * with a brighter cyan segment pointing at the talker when micd reports a
 * confident direction.  A missing or diffuse direction falls back to the
 * solid ring rather than pointing at noise.
 *
 * This is a best-effort visual cue.  It queries micd's beam snapshot (no
 * audio) once at the start of the listening window and sends one ledd
 * pattern, so it never adds a blocking audio path or a long-lived thread.
 *
 * led_socket is the ledd socket to use, and owner is the ledd pattern owner,
 * e.g. "wakeword" or "voice-listening".
 */
int le_listening_ring_show(const char *led_socket, const char *owner);

#endif
