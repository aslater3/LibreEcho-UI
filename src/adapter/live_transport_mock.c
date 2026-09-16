#define _POSIX_C_SOURCE 200809L

#include "live_transport.h"

#include <stdio.h>
#include <string.h>

/*
 * Deterministic mock GPT-Live transport.
 *
 * This is not a stub that always succeeds.  It exists so the daemon's hard
 * paths are exercised on a host with no OpenAI account and on a device with no
 * network: unexpected disconnects, duplicated delegations, slow opens, model
 * errors, and a model that simply stops talking.  The host test suite and the
 * on-device hot-stage both drive the same scenarios, so a behaviour that passes
 * on the host is the behaviour the device runs.
 *
 * Scenarios are selected by name so a test, an operator probe and the device
 * acceptance run all name the case they mean.
 */

enum mock_step {
    MOCK_STEP_OPEN = 0,
    MOCK_STEP_USER_PARTIAL,
    MOCK_STEP_USER_FINAL,
    MOCK_STEP_DELEGATION,
    MOCK_STEP_DELEGATION_REPLAY,
    MOCK_STEP_AUDIO,
    MOCK_STEP_MODEL_TRANSCRIPT,
    MOCK_STEP_OUTPUT_DONE,
    MOCK_STEP_CLOSE,
    MOCK_STEP_ERROR,
    MOCK_STEP_DONE
};

struct mock_state {
    const char *scenario;
    unsigned int step;
    unsigned int seed;
    unsigned int audio_chunks;
    unsigned int audio_sent;
    unsigned int delegation_index;
    unsigned int rate;
    unsigned int poll_calls;
    unsigned int opens;
    unsigned int closes;
    unsigned int interrupts;
    unsigned int complete_calls;
    unsigned int result_ok;
    uint64_t samples_received;
    uint64_t frames_received;
    int duplicate_delegation;
    int delay_open;
    char last_result[LE_LIVE_ARGUMENT_MAX];
    char last_delegation_id[LE_LIVE_DELEGATION_ID_MAX];
};

static struct mock_state *mock(struct le_live_transport *transport)
{
    return (struct mock_state *)transport->state;
}

static const struct mock_state *mock_const(const struct le_live_transport *t)
{
    return (const struct mock_state *)t->state;
}

static void fill_pcm(struct mock_state *state, struct le_live_event *event,
                     unsigned int chunks)
{
    /* A deterministic square wave at rate/32: audible in shape, cheap to make,
       and identical on every run so tests can assert on it. */
    static const unsigned int period = 32;
    unsigned int phase = state->audio_sent * LE_LIVE_AUDIO_SAMPLES;
    size_t i;

    event->kind = LE_LIVE_EVENT_AUDIO;
    event->rate = state->rate;
    event->count = LE_LIVE_AUDIO_SAMPLES;
    for (i = 0; i < event->count; ++i) {
        unsigned int position = (phase + (unsigned int)i) % period;

        event->samples[i] = (int16_t)(position < period / 2 ? 6000 : -6000);
    }
    (void)chunks;
}

static int mock_start(struct le_live_transport *transport,
                      const struct le_live_transport_config *config,
                      char *detail, size_t detail_size)
{
    struct mock_state *state;

    if (detail && detail_size)
        detail[0] = '\0';
    if (!transport)
        return -1;
    memset(transport->state, 0, sizeof(transport->state));
    state = mock(transport);
    state->scenario = config && config->mock_scenario
        ? config->mock_scenario : "session";
    state->seed = config ? config->mock_seed : 1U;
    state->rate = 24000U;
    state->audio_chunks = 3U;
    state->delay_open = !strcmp(state->scenario, "delay_open");
    state->duplicate_delegation = !strcmp(state->scenario, "duplicate_delegation");
    if (!strcmp(state->scenario, "silent"))
        state->audio_chunks = 0U;
    /*
     * "silent" opens and then says nothing; "unreachable" never opens at all,
     * which is the connect-timeout path.
     */
    if (!strcmp(state->scenario, "unreachable"))
        state->step = MOCK_STEP_DONE;
    if (!strcmp(state->scenario, "error")) {
        state->step = MOCK_STEP_ERROR;
        state->audio_chunks = 0U;
    }
    if (!strcmp(state->scenario, "disconnect")) {
        state->audio_chunks = 0U;
    }
    return 0;
}

static int mock_send_audio(struct le_live_transport *transport,
                           const int16_t *samples, size_t count)
{
    struct mock_state *state = mock(transport);

    if (!samples || !count || count > LE_LIVE_AUDIO_SAMPLES)
        return -1;
    ++state->frames_received;
    state->samples_received += count;
    return 0;
}

static int mock_poll(struct le_live_transport *transport,
                     struct le_live_event *event, int timeout_ms)
{
    struct mock_state *state = mock(transport);
    int result = 1;

    (void)timeout_ms;
    if (!event)
        return -1;
    memset(event, 0, sizeof(*event));
    /* Alternate "event" and "timeout" so the daemon's timeout arithmetic runs
       on the same schedule it will on the device. */
    if ((++state->poll_calls & 1U) == 0U)
        return 0;

    switch (state->scenario && !strcmp(state->scenario, "error")
                ? MOCK_STEP_ERROR
                : (enum mock_step)state->step) {
    case MOCK_STEP_OPEN:
        if (state->delay_open && state->poll_calls < 6U)
            return 0;
        ++state->opens;
        event->kind = LE_LIVE_EVENT_OPEN;
        ++state->step;
        break;
    case MOCK_STEP_USER_PARTIAL:
        /* A model that never answers, and a stream that drops mid-turn. */
        if (!strcmp(state->scenario, "silent")) {
            state->step = MOCK_STEP_DONE;
            return 0;
        }
        if (!strcmp(state->scenario, "disconnect")) {
            state->step = MOCK_STEP_CLOSE;
            return 0;
        }
        event->kind = LE_LIVE_EVENT_TRANSCRIPT;
        event->speaker = LE_LIVE_SPEAKER_USER;
        event->final = 0;
        snprintf(event->text, sizeof(event->text), "turn the kitchen");
        ++state->step;
        break;
    case MOCK_STEP_USER_FINAL:
        event->kind = LE_LIVE_EVENT_TRANSCRIPT;
        event->speaker = LE_LIVE_SPEAKER_USER;
        event->final = 1;
        snprintf(event->text, sizeof(event->text),
                 "turn the kitchen lights off");
        ++state->step;
        break;
    case MOCK_STEP_DELEGATION:
        if (!strcmp(state->scenario, "conversation_only")) {
            state->step = MOCK_STEP_AUDIO;
            return 0;
        }
        event->kind = LE_LIVE_EVENT_DELEGATION;
        snprintf(event->delegation_id, sizeof(event->delegation_id),
                 "dg-%u", state->delegation_index);
        /*
         * "device_query" delegates a read-only tool so a device without the
         * timer payload installed can still exercise a real successful
         * dispatch against a live sibling daemon.
         */
        if (!strcmp(state->scenario, "tool_denied")) {
            snprintf(event->tool, sizeof(event->tool), "shell.exec");
            snprintf(event->arguments, sizeof(event->arguments),
                     "{\"command\":\"rm -rf /\"}");
        } else if (!strcmp(state->scenario, "device_query")) {
            snprintf(event->tool, sizeof(event->tool), "device.volume");
            snprintf(event->arguments, sizeof(event->arguments), "{}");
        } else {
            snprintf(event->tool, sizeof(event->tool), "timer.set");
            snprintf(event->arguments, sizeof(event->arguments),
                     "{\"seconds\":600}");
        }
        ++state->delegation_index;
        state->step = state->duplicate_delegation
            ? MOCK_STEP_DELEGATION_REPLAY : MOCK_STEP_AUDIO;
        break;
    case MOCK_STEP_DELEGATION_REPLAY:
        /* Same id, resent: a mutating action must not run twice. */
        event->kind = LE_LIVE_EVENT_DELEGATION;
        snprintf(event->delegation_id, sizeof(event->delegation_id), "dg-0");
        snprintf(event->tool, sizeof(event->tool), "timer.set");
        snprintf(event->arguments, sizeof(event->arguments),
                 "{\"seconds\":600}");
        state->step = MOCK_STEP_AUDIO;
        break;
    case MOCK_STEP_AUDIO:
        if (state->audio_sent >= state->audio_chunks) {
            state->step = MOCK_STEP_MODEL_TRANSCRIPT;
            return 0;
        }
        fill_pcm(state, event, state->audio_chunks);
        ++state->audio_sent;
        if (state->audio_sent >= state->audio_chunks)
            state->step = MOCK_STEP_MODEL_TRANSCRIPT;
        /*
         * An interrupt means the model stops and the rest of this turn's audio
         * is never delivered; if it happens anyway the daemon has to drop it.
         */
        if (state->interrupts)
            event->count = event->count;
        break;
    case MOCK_STEP_MODEL_TRANSCRIPT:
        event->kind = LE_LIVE_EVENT_TRANSCRIPT;
        event->speaker = LE_LIVE_SPEAKER_MODEL;
        event->final = 1;
        snprintf(event->text, sizeof(event->text),
                 "Okay, the kitchen lights are off.");
        state->step = MOCK_STEP_OUTPUT_DONE;
        break;
    case MOCK_STEP_OUTPUT_DONE:
        event->kind = LE_LIVE_EVENT_OUTPUT_DONE;
        state->step = MOCK_STEP_DONE;
        break;
    case MOCK_STEP_CLOSE:
        ++state->closes;
        event->kind = LE_LIVE_EVENT_CLOSED;
        snprintf(event->detail, sizeof(event->detail), "stream closed");
        state->step = MOCK_STEP_DONE;
        break;
    case MOCK_STEP_ERROR:
        event->kind = LE_LIVE_EVENT_ERROR;
        snprintf(event->detail, sizeof(event->detail),
                 "realtime session unavailable");
        state->step = MOCK_STEP_DONE;
        break;
    case MOCK_STEP_DONE:
    default:
        result = 0;
        break;
    }

    /* A disconnect scenario tears the transport down mid-turn, without a close
       event, exactly like a dropped websocket. */
    if (result > 0 && !strcmp(state->scenario, "lost") &&
        state->step >= MOCK_STEP_AUDIO)
        return -1;
    return result;
}

static int mock_interrupt(struct le_live_transport *transport)
{
    struct mock_state *state = mock(transport);

    ++state->interrupts;
    /* Truncate the turn: whatever had not been emitted is cancelled. */
    state->audio_chunks = state->audio_sent;
    state->step = MOCK_STEP_MODEL_TRANSCRIPT;
    return 0;
}

static int mock_complete_delegation(struct le_live_transport *transport,
                                    const char *delegation_id,
                                    const char *result)
{
    struct mock_state *state = mock(transport);

    if (!delegation_id || !delegation_id[0])
        return -1;
    ++state->complete_calls;
    if (result && result[0])
        state->result_ok = strstr(result, "\"ok\":true") ? 1 : 0;
    snprintf(state->last_result, sizeof(state->last_result), "%s",
             result ? result : "");
    snprintf(state->last_delegation_id, sizeof(state->last_delegation_id),
             "%s", delegation_id);
    return 0;
}

static void mock_stop(struct le_live_transport *transport)
{
    struct mock_state *state = mock(transport);

    if (!transport)
        return;
    ++state->closes;
    state->step = MOCK_STEP_DONE;
}

static void mock_metrics(const struct le_live_transport *transport, char *out,
                         size_t size)
{
    const struct mock_state *state = mock_const(transport);
    int written;

    if (!out || !size)
        return;
    written = snprintf(
        out, size,
        "{\"scenario\":\"%s\",\"opens\":%u,\"closes\":%u,\"interrupts\":%u,"
        "\"frames_received\":%llu,\"samples_received\":%llu,"
        "\"delegation_completions\":%u,\"last_delegation_id\":\"%s\"}",
        state->scenario ? state->scenario : "",
        state->opens, state->closes, state->interrupts,
        (unsigned long long)state->frames_received,
        (unsigned long long)state->samples_received,
        state->complete_calls, state->last_delegation_id);
    if (written < 0 || (size_t)written >= size)
        out[size - 1] = '\0';
}

static const struct le_live_transport_ops mock_ops = {
    "mock",
    mock_start,
    mock_send_audio,
    mock_poll,
    mock_interrupt,
    mock_complete_delegation,
    mock_stop,
    mock_metrics
};

const struct le_live_transport_ops *le_live_transport_mock_ops(void)
{
    return &mock_ops;
}
