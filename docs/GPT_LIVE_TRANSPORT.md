# GPT-Live subscription transport (WebSocket)

Status: **specification extracted from the authoritative client; not yet
implemented in `libreecho-lived`.** This file exists so the implementation is
executing a known wire format rather than re-deriving it.

## Why this file exists

An earlier revision of `docs/GPT_LIVE.md` concluded that subscription-backed
GPT-Live required WebRTC. That was wrong, and the correction matters because it
changes the cost of the feature from "embed a WebRTC stack on a 491 MB ARMv7
device" to "write a WebSocket client".

The mistake came from reading the Codex binary's strings: `Webrtc` and
`ExistingCall` are the first two variants of `ThreadRealtimeStartTransport` that
appear in the string table, and the WebRTC path is the one that is noisy in
logs. The third variant, `ThreadRealtimeStartTransport::Websocket`, and the
`protocol_frameless_bidi` module behind it, are both present and both reachable
with the same ChatGPT account.

Source of truth: `github.com/openai/codex`, pinned at
`a6d4741d3968ee0b9984e896db08d6fc4aef05e8`, files

```
codex-rs/codex-api/src/endpoint/realtime_websocket/protocol.rs
codex-rs/codex-api/src/endpoint/realtime_websocket/protocol_frameless_bidi.rs
codex-rs/codex-api/src/endpoint/realtime_websocket/methods.rs
codex-rs/codex-api/src/endpoint/realtime_websocket/methods_frameless_bidi.rs
codex-rs/codex-api/src/endpoint/realtime_websocket/protocol_common.rs
```

## Endpoint

```
wss://<host>/v1/realtime?intent=quicksilver&model=gpt-live-1-codex
```

- The base must be scheme + host (+ optional port) with **no path segments**;
  the client refuses one that has any (`"realtime sideband URL cannot contain
  path segments"`), because `/v1/realtime` is appended.
- `intent=quicksilver` is mandatory and is what selects this protocol. In the
  tests upstream it appears as
  `wss://api.openai.com/v1/realtime?intent=quicksilver&model=snapshot`.
- `model` is optional and, for the subscription path, is `gpt-live-1-codex`.
- The base host for the ChatGPT subscription is the remaining unknown. The
  Codex client makes it configurable (`experimental_realtime_ws_base_url`) and
  its own tests only exercise `api.openai.com` and example hosts. The WebRTC
  call endpoint lives at `chatgpt.com/backend-api/realtime/calls`, but the
  sideband base must be path-free, so `wss://chatgpt.com/v1/realtime` and
  `wss://chatgpt.com/backend-api/v1/realtime` are both worth trying first.

Authentication reuses the existing ChatGPT/Codex device OAuth, exactly as the
Responses path in `llm_codex.c` already does:

```
Authorization: Bearer <access_token>
chatgpt-account-id: <account_id>
```

No new credential, no second login, no API key.

## Framing

Text frames carrying JSON, one event per frame, `type` as the discriminant.
Audio is **base64 PCM**, not Opus and not a binary frame:

- input: `audio/pcm` at the rate the device sends (16 kHz from the waked
  post-AEC stream is the natural choice)
- output: base64 PCM16 **mono at 24000 Hz** — `DEFAULT_AUDIO_SAMPLE_RATE` in
  `protocol_frameless_bidi.rs`, and the only rate the parser ever reports

## Outbound events (client to server)

```jsonc
{"type":"session.update","session":{ ... }}          // see below
{"type":"input_audio.append","audio":"<base64 pcm16>"}
{"type":"delegation.context.append","delegation_item_id":"<id>",
 "content":[{"type":"input_text","text":"<result>"}]}
{"type":"session.context.append","content":[{"type":"input_text","text":"..."}]}
{"type":"session.close"}
{"type":"response.create"}
{"type":"conversation.item.create","item":{ ... }}
```

Two other append forms exist for the framed protocols and are not needed here:
`input_audio_buffer.append` (v1-style) and `conversation.handoff.append`
(`{handoff_id, output_text}`).

Context appends are **chunked at 500 bytes on a UTF-8 boundary**
(`CONTEXT_APPEND_MAX_BYTES`), so a long delegation result is several events.

### session.update, frameless form

The frameless variant sends a free-form `session` object
(`methods_frameless_bidi::session_json`):

```jsonc
{
  "instructions": "<assistant instructions>",
  "model": "gpt-live-1-codex",                  // omitted when not set
  "audio": { "output": { "voice": "cove" } },
  "delegation": {
    "type": "client",                           // client-managed delegation
    "ack_filler": true                          // optional
  },
  "initial_items": [
    {"type":"message","role":"user",
     "content":[{"type":"input_text","text":"..."}]}
  ]
}
```

`role` maps to `input_text` for user/developer and `output_text` for assistant.

### session.update, full form

The typed variant (`SessionUpdateSession` in `protocol.rs`) additionally
supports input audio configuration and is what a device should send when it
wants to describe its own VAD instead of relying on the server default:

```jsonc
{"type":"session.update","session":{
  "type":"quicksilver",                         // quicksilver | realtime | transcription
  "model":"gpt-live-1-codex",
  "instructions":"...",
  "output_modalities":["audio"],
  "audio":{
    "input":{
      "format":{"type":"audio/pcm","rate":16000},
      "noise_reduction":{"type":"near_field"},
      "turn_detection":{"type":"server_vad","interrupt_response":true,
                        "create_response":true,"silence_duration_ms":500}
    },
    "output":{"format":{"type":"audio/pcm","rate":24000},"voice":"cove"}
  }
}}
```

`session.type = "quicksilver"` is what ties this to `intent=quicksilver`.

## Inbound events (server to client)

| `type` | payload | maps to |
|---|---|---|
| `session.started`, `session.updated` | session echo | connect complete |
| `output_audio.delta` | `{"audio":"<base64 pcm16 mono 24 kHz>"}` | playback |
| `input_transcript.added` | `{"item":{"text":"..."}}` | user transcript delta |
| `output_transcript.added` | `{"item":{"text":"..."}}` | model transcript delta |
| `turn.done` | `{"turn":{"role":"user"\|"assistant","transcript":"..."}}` | final transcript |
| `delegation.created` | see below | tool request |
| `error` | error payload | bounded failure |

Unknown types are ignored by design (the client logs and drops them), so a
new server event cannot break the device.

### delegation.created

```jsonc
{"type":"delegation.created","item":{
  "id":"<delegation item id>",
  "type":"delegation",
  "target":"client",
  "content":[{"type":"input_text","text":"turn the kitchen lights off"}]
}}
```

This is client-managed handoff and it maps directly onto the delegation layer
already built in `live_tools.c` / `live_session.c`:

1. `item.id` is the delegation id and therefore the exactly-once cache key.
2. The concatenated `input_text` content is the request.
3. The reply is `delegation.context.append` with the same id and a
   `content:[{"type":"input_text","text":"<result>"}]`, chunked at 500 bytes.

`item.type == "delegation"` and `item.target == "client"` are both required for
the client to treat it as a handoff; anything else is ignored upstream.

## What is left to build

1. A WebSocket client: HTTP `Upgrade` handshake over the repository's existing
   TLS (`src/tls.c`), then RFC 6455 framing — client-to-server frames MUST be
   masked; the server-to-client direction MUST NOT be. Bounded read/write
   buffers, no dynamic growth.
2. Base64 encode/decode, bounded, for both directions.
3. A `live_transport_websocket.c` implementing `le_live_transport_ops` on top of
   that: `session.update` on connect, `input_audio.append` per chunk,
   `delegation.context.append` for results, `session.close` on retire, and the
   inbound parser table above feeding `LE_LIVE_EVENT_AUDIO`,
   `LE_LIVE_EVENT_TRANSCRIPT`, `LE_LIVE_EVENT_DELEGATION`, `LE_LIVE_EVENT_OPEN`
   and `LE_LIVE_EVENT_OUTPUT_DONE`.
4. Output resampling 24 kHz to the 48 kHz bus — already handled by
   `live_audio_out` via `le_radio_resample`.
5. A fake server for host tests that speaks this protocol, so the parser and
   the framing are covered without an account.

## Credentials

The device currently has **no** ChatGPT credentials: `/data/libreecho/secrets/`
is empty, so `le_llm_credentials_load` returns nothing and the transport
correctly reports `"GPT-Live unavailable: sign in to ChatGPT again."`.

An end-to-end proof on hardware therefore needs the existing LibreEcho device
login to be completed once (`agentd` `auth_start` / `auth_poll`, or the login
control in the control centre). That is a user action — a code is entered at
`auth.openai.com/codex/device` — and it is the same account the Responses path
already uses. Until it is done, everything above is verifiable only against a
mock server.
