#ifndef LE_VOICE_LISTENING_LED_H
#define LE_VOICE_LISTENING_LED_H

/* Best-effort visual-only indication for the active speech-capture window. */
void le_voice_listening_led_set(int active);
/* Local-pipeline feedback: the same ring plus a chirp when capture starts. */
void le_voice_listening_feedback_set(int active);
/* Local pipeline: the owner's finished-listening chime, if they uploaded one
   (#410). There is no built-in end sound, so without one this is silent. */
void le_voice_listening_end_sound(void);

#endif
