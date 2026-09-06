#ifndef LIBREECHO_STOP_INTENT_H
#define LIBREECHO_STOP_INTENT_H

/*
 * "Stop" is the least ambiguous thing anyone says to a speaker that is making
 * noise, so the device decides what to do about it here rather than handing the
 * word to the language model. Deciding is kept separate from acting: what the
 * daemon can reach, and whether this build has timers at all, is the caller's
 * problem.
 */

/* What the device should do, given what is currently making noise. */
enum le_stop_action {
    LE_STOP_NONE = 0,   /* nothing is playing; let the model answer */
    LE_STOP_TIMER,      /* a timer is ringing: silence that and nothing else */
    LE_STOP_LOCAL,      /* stop every source this device drives */
    LE_STOP_EXTERNAL    /* only a phone-driven source: explain, cannot stop */
};

/* What is making noise right now. Fields this build cannot know stay zero. */
struct le_stop_state {
    int timer_ringing;
    int radio_playing;
    int speaking;        /* the assistant's own reply is still being spoken */
    int noise_active;
    int external_source; /* AirPlay or Bluetooth is the active source */
};

struct le_stop_plan {
    enum le_stop_action action;
    int stop_radio;
    int stop_speech;
    int stop_noise;
};

/* Fills `plan` with what to do. Safe with a NULL state or plan. */
void le_stop_intent_plan(const struct le_stop_state *state,
                         struct le_stop_plan *plan);

/*
 * True when the transcript is a request to stop. Word boundaries and negation
 * are both checked: a false hit silences whatever the person was enjoying, so
 * "stopwatch" and "don't stop" must not match.
 */
int le_stop_intent_matches(const char *transcript);

/* What to say when only a phone-driven source is playing. */
const char *le_stop_external_speech(void);

#endif /* LIBREECHO_STOP_INTENT_H */
