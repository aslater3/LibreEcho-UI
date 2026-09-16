#define _POSIX_C_SOURCE 200809L

#include "live_transport.h"
#include "llm_store.h"

#include <stdio.h>
#include <string.h>

/*
 * Subscription-backed GPT-Live transport.
 *
 * Status in 0.14: not implemented. This file says so at runtime rather than
 * pretending otherwise, and it records what the implementation actually needs.
 *
 * The requirement is a **WebSocket**, not WebRTC. An earlier revision of this
 * file claimed WebRTC and that was wrong; see docs/GPT_LIVE_TRANSPORT.md for
 * the correction and the full extracted wire format. In short, from
 * openai/codex at a6d4741d3968ee0b9984e896db08d6fc4aef05e8:
 *
 *   ThreadRealtimeStartTransport has three variants - Webrtc, ExistingCall and
 *   Websocket. The Websocket one speaks `protocol_frameless_bidi`:
 *
 *     wss://<host>/v1/realtime?intent=quicksilver&model=gpt-live-1-codex
 *
 *   authenticated with the same ChatGPT bearer and account id the Responses
 *   path already uses. Audio is base64 PCM in JSON text frames - 24 kHz mono
 *   outbound from the server, `audio/pcm` inbound - not Opus, and there is no
 *   media plane to negotiate. Delegation is client-managed:
 *   `delegation.created` in, `delegation.context.append` out.
 *
 * So the remaining work is a WebSocket client over the repository's existing
 * TLS plus base64, not an embedded WebRTC stack. Until it lands this transport
 * refuses to open a session and reports why, so that:
 *
 *   - no wake reaches a model over an unverified path;
 *   - the failure is the same bounded, speakable sentence every time, rather
 *     than a reconnect loop against an endpoint that is not implemented;
 *   - the rest of the pipeline (preroll, delegation, barge-in, exactly-once,
 *     playback) is implemented, tested and demonstrable ahead of it.
 */

#define REALTIME_WS_PATH "/v1/realtime?intent=quicksilver"

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

    if (detail && detail_size)
        detail[0] = '\0';
    if (!transport)
        return -1;
    memset(transport->state, 0, sizeof(transport->state));
    state = realtime(transport);
    ++state->attempts;

    memset(&credentials, 0, sizeof(credentials));
    /*
     * The existing device login is the only credential source. There is no
     * separate GPT-Live sign-in: if the device is signed in to ChatGPT, Live
     * must be able to use that account or say that it cannot.
     */
    if (config && config->credentials_path &&
        le_llm_credentials_load(config->credentials_path, &credentials) == 0 &&
        credentials.access_token[0])
        state->credentials_present = 1;
    le_llm_credentials_clear(&credentials);

    /*
     * Authentication is checked first because it is the failure the user can
     * act on. Only once the device is signed in does the missing transport
     * become the real reason.
     */
    if (!state->credentials_present)
        snprintf(state->detail, sizeof(state->detail),
                 "GPT-Live unavailable: sign in to ChatGPT again.");
    else
        snprintf(state->detail, sizeof(state->detail),
                 "GPT-Live needs a WebSocket transport, which this build does "
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
        "{\"implemented\":false,\"required_transport\":\"websocket\","
        "\"ws_path\":\"%s\",\"auth\":\"chatgpt-oauth\","
        "\"credentials_present\":%s,\"attempts\":%u,\"detail\":\"%s\"}",
        REALTIME_WS_PATH, state->credentials_present ? "true" : "false",
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
