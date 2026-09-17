#define _POSIX_C_SOURCE 200809L

/*
 * GPT-Live session state machine, transcript, delegation policy, timeouts,
 * barge-in and cleanup - driven by the mock transport with synthetic time.
 */

#include "adapter/live_ring.h"
#include "adapter/live_session.h"
#include "adapter/live_transport.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct harness {
    struct le_live_ring ring;
    struct le_live_session session;
    struct le_live_session_config config;
    uint64_t now;
    int dispatches;
    int denials;
    int outputs;
    int cancels;
    int dispatch_ends_session;
    char last_tool[LE_LIVE_TOOL_NAME_MAX];
    char last_arguments[LE_LIVE_ARGUMENT_MAX];
};

static int harness_dispatch(void *context, const char *tool,
                            const char *arguments, char *result, size_t size)
{
    struct harness *h = context;
    int denied = !strcmp(tool, "shell.exec");

    ++h->dispatches;
    snprintf(h->last_tool, sizeof(h->last_tool), "%s", tool ? tool : "");
    snprintf(h->last_arguments, sizeof(h->last_arguments), "%s",
             arguments ? arguments : "");
    if (denied) {
        ++h->denials;
        snprintf(result, size, "{\"ok\":false,\"error\":\"not available\"}");
        return -1;
    }
    snprintf(result, size, h->dispatch_ends_session
             ? "{\"ok\":true,\"end_session\":true}"
             : "{\"ok\":true,\"seconds\":600}");
    return 0;
}

static int harness_output(void *context, const int16_t *samples, size_t count,
                          unsigned int rate)
{
    struct harness *h = context;

    (void)samples;
    (void)count;
    (void)rate;
    ++h->outputs;
    return 0;
}

static void harness_cancel(void *context)
{
    struct harness *h = context;

    ++h->cancels;
}

static void harness_state(void *context, enum le_live_state state)
{
    (void)context;
    (void)state;
}

static const struct le_live_session_ops harness_ops = {
    harness_output, harness_cancel, harness_dispatch, harness_state
};

static void harness_init(struct harness *h, const char *scenario,
                         unsigned int timeout_ms, unsigned int max_ms)
{
    memset(h, 0, sizeof(*h));
    le_live_ring_reset(&h->ring);
    h->config.transport_ops = le_live_transport_mock_ops();
    h->config.conversation_timeout_ms = timeout_ms;
    h->config.max_session_ms = max_ms;
    h->config.connect_timeout_ms = 500;
    h->config.wake_preroll_ms = 150;
    h->config.barge_in_rms = 900;
    h->config.barge_in_factor = 6;
    h->config.barge_in_frames = 3;
    h->config.model = "gpt-live-1-codex";
    h->config.voice = "cove";
    h->config.mock_scenario = scenario;
    h->now = 1000;
    le_live_session_init(&h->session, &h->ring, &h->config, &harness_ops, h);
}

/* Deliver `milliseconds` of 16 kHz post-AEC audio in 10 ms frames. */
static int feed_ms(struct harness *h, unsigned int milliseconds,
                   unsigned int amplitude)
{
    int16_t frame[160];
    unsigned int frames = milliseconds / 10U;
    unsigned int i;
    uint64_t first = le_live_ring_end(&h->ring);
    size_t j;

    for (i = 0; i < frames; ++i) {
        for (j = 0; j < 160; ++j)
            frame[j] = (int16_t)((i + j) % 2 ? amplitude : -amplitude);
        if (le_live_session_feed(&h->session, first, frame, 160, h->now) < 0)
            return -1;
        first += 160;
    }
    return 0;
}

typedef int (*live_predicate)(struct harness *h);

static int pred_listening(struct harness *h)
{
    return h->session.state == LE_LIVE_LISTENING;
}

static int pred_idle(struct harness *h)
{
    return h->session.state == LE_LIVE_IDLE;
}

static int pred_delegated(struct harness *h)
{
    return h->session.delegation_count > 0;
}

static int pred_second_dispatch(struct harness *h)
{
    return h->dispatches >= 2;
}

static int pred_deduped(struct harness *h)
{
    return h->session.delegation_deduped > 0;
}

static int pred_denied(struct harness *h)
{
    return h->session.delegation_failures > 0;
}

static int pred_output(struct harness *h)
{
    return h->outputs > 0;
}

/*
 * Advance synthetic time in 5 ms steps until the predicate holds.  A transport
 * error is not a test failure here: the session is expected to close itself and
 * the caller asserts on the resulting state.
 */
static int pump_until(struct harness *h, unsigned int budget_ms,
                      live_predicate ready)
{
    unsigned int i;

    for (i = 0; i < budget_ms / 5U; ++i) {
        h->now += 5;
        (void)le_live_session_pump(&h->session, 0, h->now);
        if (ready(h))
            return 0;
    }
    fprintf(stderr, "pump_until: gave up at state=%s\n",
            le_live_state_name(h->session.state));
    return -1;
}

static int test_open_listen_speak_timeout(void)
{
    struct harness h;
    char transcript[4096];

    harness_init(&h, "session", 400, 60000);
    /* Fill the preroll window before the wake, as the daemon does. */
    CHECK(feed_ms(&h, 500, 50) == 0);
    CHECK(h.ring.count == 8000);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    CHECK(h.session.state == LE_LIVE_CONNECTING);
    CHECK(h.session.sessions_started == 1);
    /* The first samples sent must be ones we actually hold. */
    CHECK(h.session.next_send_sample >= le_live_ring_begin(&h.ring));

    CHECK(pump_until(&h, 500, pred_listening) == 0);
    CHECK(h.session.last_connection_ms < 500);

    /* Audio fed after the open must reach the transport. */
    CHECK(feed_ms(&h, 200, 60) == 0);
    CHECK(h.session.audio_input_ms > 0);

    CHECK(pump_until(&h, 2000, pred_delegated) == 0);
    CHECK(h.session.delegation_count == 1);
    CHECK(h.dispatches == 1);
    CHECK(!strcmp(h.last_tool, "timer.set"));
    CHECK(!strcmp(h.last_arguments, "{\"seconds\":600}"));

    CHECK(pump_until(&h, 2000, pred_output) == 0);
    CHECK(h.session.state == LE_LIVE_SPEAKING);
    CHECK(h.session.audio_output_ms > 0);
    CHECK(h.session.last_first_audio_ms > 0);

    /* Model speech ends; the session drops back to listening for a follow-up. */
    CHECK(pump_until(&h, 2000, pred_listening) == 0);

    /* Silence after the model stops closes the conversation. */
    CHECK(pump_until(&h, 2000, pred_idle) == 0);
    CHECK(h.session.last_end == LE_LIVE_END_TIMEOUT);
    CHECK(h.session.sessions_completed == 1);
    /* Closing must have dropped queued playback. */
    CHECK(h.cancels > 0);

    /* One user turn and one model turn; the partial was folded into the final. */
    le_live_session_transcript_json(&h.session, transcript, sizeof(transcript));
    CHECK(strstr(transcript, "turn the kitchen lights off") != NULL);
    CHECK(strstr(transcript, "\"role\":\"user\"") != NULL);
    CHECK(strstr(transcript, "\"role\":\"assistant\"") != NULL);
    CHECK(strstr(transcript, "Okay, the kitchen lights are off.") != NULL);
    CHECK(h.session.turn_count == 2);
    return 0;
}

static int test_duplicate_delegation_runs_once(void)
{
    struct harness h;

    harness_init(&h, "duplicate_delegation", 5000, 60000);
    CHECK(feed_ms(&h, 300, 40) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    CHECK(pump_until(&h, 3000, pred_deduped) == 0);
    /*
     * The replay carried the same id.  The mutating action ran once and the
     * replayed request was answered from the cache.
     */
    CHECK(h.dispatches == 1);
    CHECK(h.session.delegation_count == 1);
    CHECK(h.session.delegation_deduped == 1);
    return 0;
}

static int test_delegation_ids_are_scoped_to_one_session(void)
{
    struct harness h;

    harness_init(&h, "session", 5000, 60000);
    CHECK(feed_ms(&h, 300, 40) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring), h.now) == 0);
    CHECK(pump_until(&h, 3000, pred_delegated) == 0);
    CHECK(h.dispatches == 1);
    le_live_session_close(&h.session, LE_LIVE_END_STOPPED, h.now);

    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring), h.now) == 0);
    CHECK(pump_until(&h, 3000, pred_second_dispatch) == 0);
    CHECK(h.dispatches == 2);
    return 0;
}

static int test_missing_transport_fails_closed(void)
{
    struct le_live_session session;
    struct le_live_ring ring;
    struct le_live_session_config config;

    memset(&config, 0, sizeof(config));
    le_live_ring_reset(&ring);
    le_live_session_init(&session, &ring, &config, &harness_ops, NULL);
    CHECK(le_live_session_wake(&session, 0, 1000) < 0);
    return 0;
}

static int test_empty_preroll_refuses_wake(void)
{
    struct harness h;

    harness_init(&h, "session", 5000, 60000);
    CHECK(le_live_session_wake(&h.session, 0, h.now) < 0);
    CHECK(h.session.sessions_started == 0);
    CHECK(strstr(h.session.last_error, "audio") != NULL);
    return 0;
}

static int test_audio_send_failure_closes_session(void)
{
    struct harness h;

    harness_init(&h, "send_error", 5000, 60000);
    CHECK(feed_ms(&h, 200, 40) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring), h.now) == 0);
    CHECK(le_live_session_pump(&h.session, 0, h.now) < 0);
    CHECK(h.session.state == LE_LIVE_IDLE);
    CHECK(h.session.last_end == LE_LIVE_END_TRANSPORT_ERROR);
    return 0;
}

static int test_denied_tool_does_not_end_the_conversation(void)
{
    struct harness h;

    harness_init(&h, "tool_denied", 5000, 60000);
    CHECK(feed_ms(&h, 300, 40) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    CHECK(pump_until(&h, 3000, pred_denied) == 0);
    CHECK(h.denials == 1);
    /* The refusal is a result, not a fatality: playback carries on. */
    CHECK(pump_until(&h, 3000, pred_output) == 0);
    CHECK(le_live_session_active(&h.session));
    CHECK(h.session.last_end == LE_LIVE_END_NONE);
    return 0;
}

static int test_barge_in_truncates_model_speech(void)
{
    struct harness h;

    harness_init(&h, "session", 5000, 60000);
    CHECK(feed_ms(&h, 300, 40) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    CHECK(pump_until(&h, 3000, pred_output) == 0);
    CHECK(h.session.state == LE_LIVE_SPEAKING);
    {
        int cancels_before = h.cancels;

        /* Three loud 10 ms frames confirm the user is talking over the model. */
        CHECK(feed_ms(&h, 30, 20000) == 0);
        CHECK(h.session.barge_ins == 1);
        CHECK(h.cancels > cancels_before);
        CHECK(h.session.state == LE_LIVE_LISTENING);
        /* The observed peak is reported so the threshold can be set from a
           measurement rather than a guess. */
        CHECK(h.session.input_peak_rms >= 20000);
    }
    /* A single quiet frame must not count as barge-in. */
    CHECK(feed_ms(&h, 10, 20) == 0);
    CHECK(h.session.barge_ins == 1);
    return 0;
}

static int test_noisy_room_does_not_trigger_barge_in(void)
{
    struct harness h;

    harness_init(&h, "session", 5000, 60000);
    /*
     * A room that already sits around 3000 RMS, which is what this device
     * measures.  The floor has to be learned from the idle stream before the
     * wake, or the first speech frame would set it.
     */
    CHECK(feed_ms(&h, 300, 3000) == 0);
    CHECK(h.session.floor_primed == 1);
    CHECK(h.session.input_floor_rms >= 2900);
    CHECK(h.session.input_floor_rms <= 3100);

    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    CHECK(pump_until(&h, 3000, pred_output) == 0);
    CHECK(h.session.state == LE_LIVE_SPEAKING);

    /* A little louder than ambient is not a person talking over the model. */
    CHECK(feed_ms(&h, 60, 3200) == 0);
    CHECK(h.session.barge_ins == 0);
    CHECK(h.session.state == LE_LIVE_SPEAKING);

    /* A real barge-in is well clear of the room. */
    CHECK(feed_ms(&h, 40, 20000) == 0);
    CHECK(h.session.barge_ins == 1);
    CHECK(h.session.state == LE_LIVE_LISTENING);
    return 0;
}

static int test_delegation_can_end_session(void)
{
    struct harness h;

    harness_init(&h, "session", 5000, 60000);
    h.dispatch_ends_session = 1;
    CHECK(feed_ms(&h, 200, 20) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring), h.now) == 0);
    CHECK(pump_until(&h, 3000, pred_idle) == 0);
    CHECK(h.dispatches == 1);
    CHECK(h.session.last_end == LE_LIVE_END_STOPPED);
    return 0;
}

static int test_connect_timeout_recovers_to_idle(void)
{
    struct harness h;

    harness_init(&h, "unreachable", 5000, 60000);
    CHECK(feed_ms(&h, 200, 30) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    CHECK(h.session.state == LE_LIVE_CONNECTING);
    CHECK(pump_until(&h, 2000, pred_idle) == 0);
    CHECK(h.session.last_end == LE_LIVE_END_CONNECT_FAILED);
    CHECK(h.session.sessions_failed == 1);
    /*
     * Re-arming matters more than the error: the wake path must accept the
     * next utterance rather than staying blocked behind a dead session.
     */
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    CHECK(h.session.sessions_started == 2);
    le_live_session_close(&h.session, LE_LIVE_END_STOPPED, h.now);
    return 0;
}

static int test_max_session_duration_is_bounded(void)
{
    struct harness h;

    harness_init(&h, "conversation_only", 60000, 300);
    CHECK(feed_ms(&h, 200, 30) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    CHECK(pump_until(&h, 3000, pred_idle) == 0);
    CHECK(h.session.last_end == LE_LIVE_END_MAX_DURATION);
    return 0;
}

static int test_disconnect_cleans_up(void)
{
    struct harness h;

    harness_init(&h, "disconnect", 5000, 60000);
    CHECK(feed_ms(&h, 300, 40) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    CHECK(pump_until(&h, 3000, pred_idle) == 0);
    CHECK(h.session.last_end == LE_LIVE_END_TRANSPORT_CLOSED);
    CHECK(h.cancels > 0);
    CHECK(h.session.next_send_sample == 0);
    return 0;
}

static int test_lost_transport_is_an_error(void)
{
    struct harness h;

    harness_init(&h, "lost", 5000, 60000);
    CHECK(feed_ms(&h, 300, 40) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    CHECK(pump_until(&h, 3000, pred_idle) == 0);
    CHECK(h.session.last_end == LE_LIVE_END_TRANSPORT_ERROR);
    CHECK(h.session.sessions_failed == 1);
    return 0;
}

static int test_model_error_event_is_bounded(void)
{
    struct harness h;

    harness_init(&h, "error", 5000, 60000);
    CHECK(feed_ms(&h, 200, 30) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    CHECK(pump_until(&h, 2000, pred_idle) == 0);
    CHECK(h.session.last_end == LE_LIVE_END_TRANSPORT_ERROR);
    CHECK(h.session.last_error[0] != '\0');
    return 0;
}

static int test_second_wake_does_not_start_two_sessions(void)
{
    struct harness h;

    harness_init(&h, "silent", 5000, 60000);
    CHECK(feed_ms(&h, 200, 30) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    /* Only one assistant pipeline may own a wake-triggered conversation. */
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 1);
    CHECK(h.session.sessions_started == 1);
    le_live_session_close(&h.session, LE_LIVE_END_STOPPED, h.now);
    return 0;
}

static int test_closing_while_idle_is_safe(void)
{
    struct harness h;

    harness_init(&h, "session", 5000, 60000);
    le_live_session_close(&h.session, LE_LIVE_END_STOPPED, h.now);
    CHECK(h.session.state == LE_LIVE_IDLE);
    CHECK(h.session.sessions_completed == 0);
    CHECK(h.cancels == 0);
    return 0;
}

static int test_status_and_transcript_are_bounded(void)
{
    struct harness h;
    char status[2048];
    char transcript[256];

    harness_init(&h, "session", 400, 60000);
    CHECK(feed_ms(&h, 200, 30) == 0);
    CHECK(le_live_session_wake(&h.session, le_live_ring_end(&h.ring),
                               h.now) == 0);
    (void)pump_until(&h, 4000, pred_idle);
    snprintf(h.session.last_error, sizeof(h.session.last_error),
             "bad \"quote\"\nline");

    le_live_session_status_json(&h.session, status, sizeof(status));
    CHECK(strstr(status, "\"state\":\"idle\"") != NULL);
    CHECK(strstr(status, "bad \\\"quote\\\"\\nline") != NULL);
    CHECK(strstr(status, "\"sessions_started\":1") != NULL);
    /* Status must never carry speech content or credentials. */
    CHECK(strstr(status, "kitchen") == NULL);
    CHECK(strstr(status, "Bearer") == NULL);
    CHECK((int)strlen(status) < (int)sizeof(status));

    /* A short buffer must truncate safely, not overflow. */
    le_live_session_transcript_json(&h.session, transcript, sizeof(transcript));
    CHECK((int)strlen(transcript) < (int)sizeof(transcript));

    CHECK(h.session.turn_count <= LE_LIVE_TRANSCRIPT_TURNS);
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*run)(void);
    } tests[] = {
        {"open/listen/speak/timeout", test_open_listen_speak_timeout},
        {"duplicate delegation", test_duplicate_delegation_runs_once},
        {"delegation ids per session", test_delegation_ids_are_scoped_to_one_session},
        {"missing transport", test_missing_transport_fails_closed},
        {"empty preroll", test_empty_preroll_refuses_wake},
        {"audio send failure", test_audio_send_failure_closes_session},
        {"denied tool", test_denied_tool_does_not_end_the_conversation},
        {"barge-in", test_barge_in_truncates_model_speech},
        {"noisy room", test_noisy_room_does_not_trigger_barge_in},
        {"delegation ends session", test_delegation_can_end_session},
        {"connect timeout", test_connect_timeout_recovers_to_idle},
        {"max session duration", test_max_session_duration_is_bounded},
        {"disconnect cleanup", test_disconnect_cleans_up},
        {"lost transport", test_lost_transport_is_an_error},
        {"model error", test_model_error_event_is_bounded},
        {"single wake owner", test_second_wake_does_not_start_two_sessions},
        {"close while idle", test_closing_while_idle_is_safe},
        {"bounded status/transcript", test_status_and_transcript_are_bounded}
    };
    size_t i;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i].run()) {
            fprintf(stderr, "live session: FAILED %s\n", tests[i].name);
            return 1;
        }
    }
    printf("live session: ok\n");
    return 0;
}
