#define _POSIX_C_SOURCE 200809L

#include "live_transport.h"
#include "llm_store.h"

#include <stdio.h>
#include <string.h>

/*
 * Subscription-backed GPT-Live transport.
 *
 * Status in 0.14: not implemented, and this file says so at runtime instead of
 * pretending otherwise.
 *
 * The transport question was investigated before any of this was written, and
 * the answer is not the one the plan hoped for.  Codex (codex-cli 0.154.0,
 * the shipped client on this machine) carries the following in its own binary:
 *
 *   codex-api/src/endpoint/realtime_call.rs
 *   codex-api/src/endpoint/realtime_websocket/protocol_v1.rs
 *   codex-api/src/endpoint/realtime_websocket/protocol_v2.rs
 *   core/src/realtime_conversation/sideband.rs
 *   core/src/realtime_conversation/existing_call.rs
 *   struct variant ThreadRealtimeStartTransport::Webrtc with 1 element
 *   struct ThreadRealtimeStartTransport::ExistingCall with 1 element
 *   "realtime sideband websocket connect failed; retrying: "
 *   "realtime call request SDP: "
 *
 * A realtime call is created over HTTPS against
 * https://chatgpt.com/backend-api/realtime/calls with `application/sdp`, the
 * ChatGPT bearer and account header, and the answer carries a call id in the
 * Location header.  Media then flows over WebRTC (SDP, ICE/STUN, DTLS, SRTP,
 * Opus), and a *sideband WebSocket* carries control events.  The sideband is
 * useless without the WebRTC call it belongs to.
 *
 * The published failure mode matches: the standalone realtime WebSocket is
 * API-key authenticated only, so it is not a way to reuse a ChatGPT
 * subscription, and pointing the ChatGPT credential at it fails.
 *
 * So the honest 0.14 answer to "what is the minimum ARM32 transport?" is:
 * WebRTC, with a sideband WebSocket.  That is a real dependency on this device
 * (ARMv7, ~491 MB RAM, musl), and embedding it is explicitly out of scope for
 * this change.  Until it lands, this transport refuses to open a session and
 * reports why, so:
 *
 *   - no wake ever reaches a model over an unverified path;
 *   - the failure is the same bounded, speakable error every time, rather than
 *     a reconnect loop against an endpoint that will keep rejecting us;
 *   - the rest of the pipeline (preroll, delegation, barge-in, playback,
 *     exactly-once) is implemented, tested and demonstrable ahead of it.
 */

#define REALTIME_CALL_URL "https://chatgpt.com/backend-api/realtime/calls"

struct realtime_state {
    /* Whatever the reason, it is one bounded sentence for the user. */
    char detail[LE_LIVE_TEXT_MAX];
    int credentials_present;
    unsigned int attempts;
};

static struct realtime_state *realtime(struct le_live_transport *transport)
{
    return (struct realtime_state *)transport->state;
}

static int realtime_start(struct le_live_transport *transport,
                          const struct le_live_transport_config *config,
                          char *detail, size_t detail_size)
{
    struct realtime_state *state;
    struct le_llm_credentials credentials;

    if (!transport)
        return -1;
    memset(transport->state, 0, sizeof(transport->state));
    state = realtime(transport);
    ++state->attempts;

    memset(&credentials, 0, sizeof(credentials));
    /*
     * The existing device login is the only credential source.  There is no
     * separate GPT-Live sign-in: if the device is signed in to ChatGPT, Live
     * must be able to use that account or say that it cannot.
     */
    if (config && config->credentials_path &&
        le_llm_credentials_load(config->credentials_path, &credentials) == 0 &&
        credentials.access_token[0])
        state->credentials_present = 1;
    le_llm_credentials_clear(&credentials);

    if (!state->credentials_present)
        snprintf(state->detail, sizeof(state->detail),
                 "GPT-Live unavailable: sign in to ChatGPT again.");
    else
        snprintf(state->detail, sizeof(state->detail),
                 "GPT-Live needs a WebRTC transport, which this build does "
                 "not include yet.");
    if (detail && detail_size)
        snprintf(detail, detail_size, "%s", state->detail);
    return -1;
}

static int realtime_send_audio(struct le_live_transport *transport,
                               const int16_t *samples, size_t count)
{
    (void)transport;
    (void)samples;
    (void)count;
    return -1;
}

static int realtime_poll(struct le_live_transport *transport,
                         struct le_live_event *event, int timeout_ms)
{
    struct realtime_state *state = realtime(transport);

    (void)timeout_ms;
    if (!event)
        return -1;
    memset(event, 0, sizeof(*event));
    event->kind = LE_LIVE_EVENT_ERROR;
    snprintf(event->detail, sizeof(event->detail), "%s", state->detail);
    return 1;
}

static int realtime_interrupt(struct le_live_transport *transport)
{
    (void)transport;
    return -1;
}

static int realtime_complete_delegation(struct le_live_transport *transport,
                                        const char *delegation_id,
                                        const char *result)
{
    (void)transport;
    (void)delegation_id;
    (void)result;
    return -1;
}

static void realtime_stop(struct le_live_transport *transport)
{
    (void)transport;
}

static void realtime_metrics(const struct le_live_transport *transport,
                             char *out, size_t size)
{
    const struct realtime_state *state =
        (const struct realtime_state *)transport->state;
    int written;

    if (!out || !size)
        return;
    written = snprintf(
        out, size,
        "{\"implemented\":false,\"required_transport\":\"webrtc\","
        "\"call_url\":\"%s\",\"credentials_present\":%s,\"attempts\":%u,"
        "\"detail\":\"%s\"}",
        REALTIME_CALL_URL, state->credentials_present ? "true" : "false",
        state->attempts, state->detail);
    if (written < 0 || (size_t)written >= size)
        out[size - 1] = '\0';
}

static const struct le_live_transport_ops realtime_ops = {
    "realtime",
    realtime_start,
    realtime_send_audio,
    realtime_poll,
    realtime_interrupt,
    realtime_complete_delegation,
    realtime_stop,
    realtime_metrics
};

const struct le_live_transport_ops *le_live_transport_realtime_ops(void)
{
    return &realtime_ops;
}
