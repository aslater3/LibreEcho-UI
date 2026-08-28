#ifndef LE_TIMER_INTENT_H
#define LE_TIMER_INTENT_H

/*
 * Recognising timer requests in a transcript, before the language model sees
 * it.
 *
 * A model can answer a question but it cannot start a timer, and asking one to
 * emit a tool call costs a network round trip for a request the device can
 * satisfy on its own. Matching here makes "set a timer for ten minutes" work
 * offline, in a few microseconds instead of a second or two, and gives the
 * same answer every time. Anything not recognised falls through to the model
 * untouched, so this only ever adds capability.
 *
 * Deliberately narrow. This matches the handful of phrasings people actually
 * use for timers and declines everything else, because a greedy matcher that
 * swallows a question the model could have answered is worse than no matcher.
 */

#include <stddef.h>

#define LE_TIMER_INTENT_LABEL_MAX 48
#define LE_TIMER_INTENT_SPEECH_MAX 160

enum le_timer_intent_kind {
    /* Not a timer request; hand the transcript to the model. */
    LE_TIMER_INTENT_NONE = 0,
    LE_TIMER_INTENT_SET,
    LE_TIMER_INTENT_CANCEL,
    /* Silence what is ringing. Only meaningful while something rings; the
       caller decides, because "stop" means something else the rest of the
       time. */
    LE_TIMER_INTENT_DISMISS,
    LE_TIMER_INTENT_QUERY
};

struct le_timer_intent {
    enum le_timer_intent_kind kind;
    long long seconds;
    int cancel_all;
    char label[LE_TIMER_INTENT_LABEL_MAX];
};

/*
 * Parse a transcript. Returns the intent kind and fills `intent`.
 * `transcript` is not modified.
 */
enum le_timer_intent_kind le_timer_intent_parse(const char *transcript,
                                                struct le_timer_intent *intent);

/*
 * The line to speak back for a recognised intent, given what actually
 * happened. `count` is the number of timers affected or remaining, depending
 * on the intent; `seconds` is the time left for a query.
 * Returns the number of characters written.
 */
int le_timer_intent_speech(const struct le_timer_intent *intent, int count,
                           long long seconds, char *out, size_t size);

/* Spell a duration the way a person says it: "ten minutes", "one hour and
   five minutes". Exposed because both the confirmation and the query need
   it, and because it is worth testing on its own. */
int le_timer_intent_say_duration(long long seconds, char *out, size_t size);

#endif
