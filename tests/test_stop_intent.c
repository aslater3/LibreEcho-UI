/*
 * What "stop" should do, given what is currently making noise.
 *
 * Saying "stop" to a device that is playing something is the least ambiguous
 * request a person makes of it, and until now it was the one the assistant
 * handled worst: the word reached the language model, which asked what to stop,
 * which opened a follow-up listen, which fed the next "stop" back in as
 * conversation. These cases pin the decision so that never happens again.
 *
 * The planner is deliberately pure. Whether the device can reach radiod, or
 * whether this build has timers at all, is the caller's problem; the rules for
 * what ought to happen are the part worth testing off-device.
 */

#include "adapter/stop_intent.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct le_stop_plan plan_for(struct le_stop_state state)
{
    struct le_stop_plan plan;

    le_stop_intent_plan(&state, &plan);
    return plan;
}

int main(void)
{
    struct le_stop_state state;
    struct le_stop_plan plan;

    /* Silence. Nothing to stop, so the model answers as it always did. */
    memset(&state, 0, sizeof(state));
    plan = plan_for(state);
    assert(plan.action == LE_STOP_NONE);
    assert(!plan.stop_radio && !plan.stop_speech && !plan.stop_noise);

    /* Each source on its own stops only itself. */
    memset(&state, 0, sizeof(state));
    state.radio_playing = 1;
    plan = plan_for(state);
    assert(plan.action == LE_STOP_LOCAL);
    assert(plan.stop_radio && !plan.stop_speech && !plan.stop_noise);

    memset(&state, 0, sizeof(state));
    state.speaking = 1;
    plan = plan_for(state);
    assert(plan.action == LE_STOP_LOCAL);
    assert(!plan.stop_radio && plan.stop_speech && !plan.stop_noise);

    memset(&state, 0, sizeof(state));
    state.noise_active = 1;
    plan = plan_for(state);
    assert(plan.action == LE_STOP_LOCAL);
    assert(!plan.stop_radio && !plan.stop_speech && plan.stop_noise);

    /*
     * Several at once stop together. "Stop" means make it quiet; choosing a
     * winner between the radio and a talking assistant would be a guess the
     * person then has to learn.
     */
    memset(&state, 0, sizeof(state));
    state.radio_playing = 1;
    state.speaking = 1;
    state.noise_active = 1;
    plan = plan_for(state);
    assert(plan.action == LE_STOP_LOCAL);
    assert(plan.stop_radio && plan.stop_speech && plan.stop_noise);

    /*
     * A ringing timer takes the word for itself and leaves the music alone.
     * An alarm going off over a song is the one case where "stop" is clearly
     * about the alarm, and silencing the song too would be a nasty surprise.
     */
    memset(&state, 0, sizeof(state));
    state.timer_ringing = 1;
    plan = plan_for(state);
    assert(plan.action == LE_STOP_TIMER);
    assert(!plan.stop_radio && !plan.stop_speech && !plan.stop_noise);

    memset(&state, 0, sizeof(state));
    state.timer_ringing = 1;
    state.radio_playing = 1;
    state.speaking = 1;
    state.noise_active = 1;
    plan = plan_for(state);
    assert(plan.action == LE_STOP_TIMER);
    assert(!plan.stop_radio && !plan.stop_speech && !plan.stop_noise);

    /*
     * AirPlay and Bluetooth are driven by the phone that started them. The
     * device cannot stop them, so it says so rather than silently doing
     * nothing -- which is exactly the failure this whole change exists to fix.
     */
    memset(&state, 0, sizeof(state));
    state.external_source = 1;
    plan = plan_for(state);
    assert(plan.action == LE_STOP_EXTERNAL);
    assert(!plan.stop_radio && !plan.stop_speech && !plan.stop_noise);

    /*
     * If something we own is also playing, stop that and stay quiet about the
     * phone: the request was satisfiable, so an explanation would be noise.
     */
    memset(&state, 0, sizeof(state));
    state.external_source = 1;
    state.radio_playing = 1;
    plan = plan_for(state);
    assert(plan.action == LE_STOP_LOCAL);
    assert(plan.stop_radio);

    /* A timer still outranks a phone-driven source. */
    memset(&state, 0, sizeof(state));
    state.external_source = 1;
    state.timer_ringing = 1;
    plan = plan_for(state);
    assert(plan.action == LE_STOP_TIMER);

    /* The spoken explanation must name the phone and be a real sentence. */
    {
        const char *speech = le_stop_external_speech();

        assert(speech && speech[0]);
        assert(strstr(speech, "phone") != NULL);
        assert(strlen(speech) < 200);
    }

    /* A missing plan or state must not crash the daemon. */
    le_stop_intent_plan(NULL, &plan);
    assert(plan.action == LE_STOP_NONE);
    memset(&state, 0, sizeof(state));
    state.radio_playing = 1;
    le_stop_intent_plan(&state, NULL);


    /*
     * Recognising the request. This build has no timer daemon and so nothing
     * that already parses "stop", which is why the matcher lives here.
     */
    assert(le_stop_intent_matches("stop"));
    assert(le_stop_intent_matches("Stop."));
    assert(le_stop_intent_matches("stop the music"));
    assert(le_stop_intent_matches("stop playing"));
    assert(le_stop_intent_matches("shut up"));
    assert(le_stop_intent_matches("be quiet"));
    assert(le_stop_intent_matches("quiet please"));
    assert(le_stop_intent_matches("silence"));
    assert(le_stop_intent_matches("turn the music off"));
    assert(le_stop_intent_matches("turn off the music"));

    /*
     * "stopwatch" contains the word but is not the request, and a negated
     * "stop" is the opposite of it. Both would be maddening as false hits,
     * because a wrong match silences whatever the person was enjoying.
     */
    assert(!le_stop_intent_matches("stopwatch"));
    assert(!le_stop_intent_matches("set a stopwatch"));
    assert(!le_stop_intent_matches("don't stop"));
    assert(!le_stop_intent_matches("do not stop the music"));
    assert(!le_stop_intent_matches("never stop"));
    assert(!le_stop_intent_matches(""));
    assert(!le_stop_intent_matches(NULL));
    assert(!le_stop_intent_matches("what time is it"));
    assert(!le_stop_intent_matches("play some jazz"));
    /* "turn it off" alone is as likely to mean the lights, so it must not hit. */
    assert(!le_stop_intent_matches("turn it off"));

    puts("stop_intent: phrases, sources, timer priority, phone sources and safety ok");
    return 0;
}
