#ifndef LE_VOICE_LISTENING_LED_H
#define LE_VOICE_LISTENING_LED_H

/* Best-effort visual-only indication for the active speech-capture window. */
void le_voice_listening_led_set(int active);
/* Local-pipeline feedback: the same ring plus a chirp when capture starts. */
void le_voice_listening_feedback_set(int active);

#endif
