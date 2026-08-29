/*
 * Recognising timer requests in a transcript.
 *
 * Two failure modes matter and they pull against each other. Missing a real
 * request sends "set a timer for ten minutes" to a language model that cannot
 * start one. Matching too eagerly steals a question the model could have
 * answered and replies with something about timers instead. The second is
 * worse, so the "not a timer request" cases below are the important half of
 * this file.
 */

#include "adapter/timer_intent.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static enum le_timer_intent_kind kind_of(const char *text)
{
    struct le_timer_intent intent;

    return le_timer_intent_parse(text, &intent);
}

static long long seconds_of(const char *text)
{
    struct le_timer_intent intent;

    if (le_timer_intent_parse(text, &intent) != LE_TIMER_INTENT_SET)
        return -1;
    return intent.seconds;
}

static void expect_seconds(const char *text, long long want)
{
    long long got = seconds_of(text);

    if (got != want) {
        fprintf(stderr, "\"%s\": wanted %lld seconds, got %lld\n", text, want,
                got);
        fflush(stderr);
        assert(got == want);
    }
}

int main(void)
{
    /* The phrasings people actually use, in digits and in words, because
       speech-to-text writes "ten" rather than "10". */
    expect_seconds("set a timer for ten minutes", 600);
    expect_seconds("set a timer for 10 minutes", 600);
    expect_seconds("Alexa, set a timer for 5 minutes.", 300);
    expect_seconds("set a 20 minute timer", 1200);
    expect_seconds("timer for two hours", 7200);
    expect_seconds("start a timer for thirty seconds", 30);
    expect_seconds("set a timer for one hour and ten minutes", 4200);
    expect_seconds("set a timer for one hour and thirty seconds", 3630);
    expect_seconds("set a timer for twenty five minutes", 1500);
    /* "a minute" is a count of one, not a missing number. */
    expect_seconds("set a timer for a minute", 60);
    expect_seconds("set a timer for an hour", 3600);
    expect_seconds("set a timer for half an hour", 1800);
    expect_seconds("set a timer for an hour and a half", 5400);

    /* Punctuation and case must not matter; a transcript has both. */
    expect_seconds("SET A TIMER FOR 3 MINUTES!", 180);

    /*
     * Not timer requests. Each of these would be answered badly if the
     * matcher took it, and each is a thing someone would really say.
     */
    assert(kind_of("what time is it") == LE_TIMER_INTENT_NONE);
    assert(kind_of("how many minutes are in a year") == LE_TIMER_INTENT_NONE);
    assert(kind_of("how long does it take to boil an egg") ==
           LE_TIMER_INTENT_NONE);
    assert(kind_of("tell me about the hour hand on a clock") ==
           LE_TIMER_INTENT_NONE);
    assert(kind_of("play some music") == LE_TIMER_INTENT_NONE);
    assert(kind_of("") == LE_TIMER_INTENT_NONE);
    assert(kind_of("what is the weather in ten minutes") ==
           LE_TIMER_INTENT_NONE);
    assert(kind_of("don't set a timer for ten minutes") ==
           LE_TIMER_INTENT_NONE);
    assert(kind_of("I don't want a ten minute timer") ==
           LE_TIMER_INTENT_NONE);
    assert(kind_of("never start an alarm for one hour") ==
           LE_TIMER_INTENT_NONE);

    /* Silencing a ring. Short, and what everyone says. */
    assert(kind_of("stop") == LE_TIMER_INTENT_DISMISS);
    assert(kind_of("Alexa, stop!") == LE_TIMER_INTENT_DISMISS);
    assert(kind_of("dismiss") == LE_TIMER_INTENT_DISMISS);
    assert(kind_of("turn off the timer") == LE_TIMER_INTENT_DISMISS);
    assert(kind_of("snooze") == LE_TIMER_INTENT_NONE);
    assert(kind_of("snooze the timer") == LE_TIMER_INTENT_NONE);

    /* Cancelling something that has not gone off is a different request. */
    assert(kind_of("cancel my timer") == LE_TIMER_INTENT_CANCEL);
    assert(kind_of("cancel all timers") == LE_TIMER_INTENT_CANCEL);
    assert(kind_of("delete the timer") == LE_TIMER_INTENT_CANCEL);

    /* Only genuinely universal wording may select cancel-all. A quantity such
       as one or two timers is still a bounded, non-universal request. */
    {
        struct le_timer_intent intent;

        assert(le_timer_intent_parse("cancel the pasta timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(!strcmp(intent.label, "pasta"));
        assert(le_timer_intent_parse("delete the tea timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(!strcmp(intent.label, "tea"));
        assert(le_timer_intent_parse("remove the coffee timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(!strcmp(intent.label, "coffee"));
        assert(le_timer_intent_parse("clear the kitchen alarm", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(!strcmp(intent.label, "kitchen"));
        assert(le_timer_intent_parse("cancel the all hands timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(!strcmp(intent.label, "all hands"));
        assert(le_timer_intent_parse("cancel the ten minute timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(intent.label[0] == '\0');
        assert(le_timer_intent_parse("clear the 10 minute alarm", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(intent.label[0] == '\0');
        assert(le_timer_intent_parse("cancel timer called fifteen minutes",
                                     &intent) == LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(intent.label[0] == '\0');
        assert(le_timer_intent_parse("remove 2 timers", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(intent.label[0] == '\0');
        assert(le_timer_intent_parse("cancel every question about timers",
                                     &intent) == LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(le_timer_intent_parse("cancel my timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(intent.label[0] == '\0');
        assert(le_timer_intent_parse("cancel one timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(intent.label[0] == '\0');
        assert(le_timer_intent_parse("cancel two timers", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(intent.label[0] == '\0');
        assert(le_timer_intent_parse("don't cancel my timer", &intent) ==
               LE_TIMER_INTENT_NONE);
        assert(le_timer_intent_parse("do not cancel all timers", &intent) ==
               LE_TIMER_INTENT_NONE);
        /* A numeric first word is not enough to classify a stored name as a
           quantity: "two eggs" is a legitimate timer label. */
        assert(le_timer_intent_parse("cancel the two eggs timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(!strcmp(intent.label, "two eggs"));
        assert(le_timer_intent_parse("cancel the 2 fast eggs alarm", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(!strcmp(intent.label, "2 fast eggs"));
        assert(le_timer_intent_parse("cancel the all timers timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(!strcmp(intent.label, "all timers"));
        assert(le_timer_intent_parse("cancel the do not disturb timer",
                                     &intent) == LE_TIMER_INTENT_CANCEL);
        assert(!intent.cancel_all);
        assert(!strcmp(intent.label, "do not disturb"));
        assert(le_timer_intent_parse("cancel all timers", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(intent.cancel_all);
        assert(le_timer_intent_parse("delete every timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(intent.cancel_all);
        assert(le_timer_intent_parse("remove each timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(intent.cancel_all);
        assert(le_timer_intent_parse("cancel every one of my timers", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(intent.cancel_all);
        assert(le_timer_intent_parse("delete each one of the alarms", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(intent.cancel_all);
        assert(le_timer_intent_parse("cancel every single timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        assert(intent.cancel_all);
        assert(le_timer_intent_parse("delete each single one of my alarms",
                                     &intent) == LE_TIMER_INTENT_CANCEL);
        assert(intent.cancel_all);
    }

    /* Asking about them. */
    assert(kind_of("how long is left on my timer") == LE_TIMER_INTENT_QUERY);
    assert(kind_of("how much time is left on the timer") ==
           LE_TIMER_INTENT_QUERY);
    assert(kind_of("what timers do i have") == LE_TIMER_INTENT_QUERY);

    /* "set a timer" with no length: asking is better than guessing. */
    {
        struct le_timer_intent intent;
        char speech[LE_TIMER_INTENT_SPEECH_MAX];

        assert(le_timer_intent_parse("set a timer", &intent) ==
               LE_TIMER_INTENT_SET);
        assert(intent.seconds == 0);
        le_timer_intent_speech(&intent, 0, 0, speech, sizeof(speech));
        assert(strstr(speech, "How long") != NULL);
    }

    /* A label is taken when it is a name, never when it is the duration. */
    {
        struct le_timer_intent intent;

        assert(le_timer_intent_parse("set a timer for the pasta for ten "
                                     "minutes", &intent) ==
               LE_TIMER_INTENT_SET);
        assert(intent.seconds == 600);
        assert(!strcmp(intent.label, "pasta"));

        assert(le_timer_intent_parse("set a timer for ten minutes",
                                     &intent) == LE_TIMER_INTENT_SET);
        assert(intent.label[0] == '\0');
    }

    /* Durations are spoken the way a person says them. */
    {
        char out[64];

        le_timer_intent_say_duration(600, out, sizeof(out));
        assert(!strcmp(out, "10 minutes"));
        le_timer_intent_say_duration(60, out, sizeof(out));
        assert(!strcmp(out, "1 minute"));
        le_timer_intent_say_duration(3600, out, sizeof(out));
        assert(!strcmp(out, "1 hour"));
        le_timer_intent_say_duration(4200, out, sizeof(out));
        assert(!strcmp(out, "1 hour and 10 minutes"));
        le_timer_intent_say_duration(3630, out, sizeof(out));
        assert(!strcmp(out, "1 hour and 30 seconds"));
        le_timer_intent_say_duration(90, out, sizeof(out));
        assert(!strcmp(out, "1 minute and 30 seconds"));
        le_timer_intent_say_duration(30, out, sizeof(out));
        assert(!strcmp(out, "30 seconds"));
    }

    /* What gets said back. */
    {
        struct le_timer_intent intent;
        char speech[LE_TIMER_INTENT_SPEECH_MAX];

        assert(le_timer_intent_parse("set a timer for ten minutes",
                                     &intent) == LE_TIMER_INTENT_SET);
        le_timer_intent_speech(&intent, 1, 0, speech, sizeof(speech));
        assert(strstr(speech, "10 minutes") != NULL);

        assert(le_timer_intent_parse("set a timer for one hour and thirty "
                                     "seconds", &intent) == LE_TIMER_INTENT_SET);
        le_timer_intent_speech(&intent, 1, 0, speech, sizeof(speech));
        assert(strstr(speech, "1 hour and 30 seconds") != NULL);

        assert(le_timer_intent_parse("cancel my timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        /* Nothing to cancel must not claim something was cancelled. */
        le_timer_intent_speech(&intent, 0, 0, speech, sizeof(speech));
        assert(strstr(speech, "no timers") != NULL);
        le_timer_intent_speech(&intent, 1, 0, speech, sizeof(speech));
        assert(strstr(speech, "cancelled") != NULL);

        assert(le_timer_intent_parse("cancel my timer", &intent) ==
               LE_TIMER_INTENT_CANCEL);
        le_timer_intent_speech(&intent, -1, 0, speech, sizeof(speech));
        assert(strstr(speech, "Which timer") != NULL);

        assert(le_timer_intent_parse("how long is left on my timer",
                                     &intent) == LE_TIMER_INTENT_QUERY);
        le_timer_intent_speech(&intent, 0, 0, speech, sizeof(speech));
        assert(strstr(speech, "no timers") != NULL);
        le_timer_intent_speech(&intent, 1, 125, speech, sizeof(speech));
        assert(strstr(speech, "2 minutes and 5 seconds") != NULL);
    }

    /* A long transcript must not overflow anything. */
    {
        char big[1024];
        struct le_timer_intent intent;

        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        (void)le_timer_intent_parse(big, &intent);
    }

    printf("timer intent: ok\n");
    return 0;
}
