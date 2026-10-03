# GPT-Live voice assistant mode

`libreecho-lived` is the daemon behind the GPT-Live assistant mode: a
speech-to-speech conversation that starts on the local wake word and can
delegate device actions back to LibreEcho.

Status in 0.14: **pipeline and subscription WebSocket transport implemented;
mock-server path device-validated.** The remaining external gate is one real
ChatGPT device login and acceptance by the live service. This document records
the transport finding, scope and hardware measurements so none of it has to be
re-derived from scratch.

## What runs

```
micd ─► waked ── wake_detected ──────────────┐
          │                                  ▼
          └── post-AEC PCM ──────────► libreecho-lived
              (16 kHz mono,               │      │
               sample-indexed)            │      └─► delegation allow-list
                                          │            │
                                          ▼            ▼
                                     GPT-Live     timer / radio / audio /
                                     transport     device sockets
                                          │
                                          ▼
                                   model audio
                                          │
                                          ▼
                            /run/libreecho-audio/system.pcm
                                          │
                                          ▼
                                        audiod
```

`lived` owns no hardware. It subscribes to the stream and the wake events
`waked` already produces, keeps a few seconds of that audio in RAM, opens a
conversation when the wake word fires, and writes model audio back through the
central playback bus so volume, mute, AirPlay arbitration and the AEC reference
stay inside `audiod`.

It never opens ALSA, never runs wake inference, never runs local STT or TTS,
and never executes a command the model asked for.

The rate conversion on the way out is not reimplemented: `lived` uses the
repository's existing shared resampler (`le_radio_resample`), which already
carries phase and history across blocks and has its own tests. A second
converter would be a second place for the channel mapping to be wrong. It
targets the platform bus rate, so a device configured for a different speaker
rate gets silence and a bounded error rather than audio at the wrong pitch.
(Wyoming's own inline conversion is unchanged; folding it onto the shared
resampler needs per-stream reset semantics its event handling does not model
yet, so that stays a separate change.)

## The transport question, answered

The plan's preferred transport was a plain TLS WebSocket reusing the existing
ChatGPT/Codex OAuth account. **That transport exists** and is what the
implementation targets; the full wire format is in
`docs/GPT_LIVE_TRANSPORT.md`.

The subscription path is a WebSocket at

```
wss://<host>/v1/realtime?intent=quicksilver&model=gpt-live-1-codex
```

authenticated with the same ChatGPT bearer and account id the Responses path
already uses. Audio is base64 PCM inside JSON text frames, 24 kHz mono outbound
from the server, with no media plane to negotiate. Delegation is
client-managed: `delegation.created` in, `delegation.context.append` out.

WebRTC is a *different*, third transport in the same client
(`ThreadRealtimeStartTransport::Webrtc`, used by the Codex desktop voice path)
and is not required here. An earlier revision of this document claimed it was,
on the strength of the binary's string table, where `Webrtc` and `ExistingCall`
appear and `Websocket` is easy to miss; the correction is recorded in
`docs/GPT_LIVE_TRANSPORT.md` because the cost difference is the whole feature.

What remains is a WebSocket client over the repository's existing TLS
(`src/tls.c`) plus base64, not an embedded WebRTC stack.

`libreecho-lived` implements that WebSocket with bounded RFC 6455 framing,
base64 PCM, verified TLS, a libc-independent DNS A resolver, session update,
audio streaming and client-managed delegation. It fails closed before sending
a bearer when credentials, DNS, TLS verification or the WebSocket handshake
are unavailable. On the current device, which has no ChatGPT credentials, the
actionable result remains `sign in to ChatGPT again`.

## What is implemented

| Area | State |
|---|---|
| Sample-indexed preroll ring (3 s, 96 KB, RAM only) | done |
| Wake-sample alignment so the first word is not clipped | done |
| Session state machine with connect / conversation / maximum-duration bounds | done |
| Continuous full-duplex input forwarding | done |
| Model audio into the central playback bus, cancellable | done |
| Barge-in with relative (self-calibrating) threshold | done |
| Bounded transcript (10 turns, never written to disk) | done |
| Delegation allow-list with argument validation | done |
| Exactly-once delegation (bounded id→result cache) | done |
| Bounded status/metrics over `/run/libreecho/live.sock` | done |
| Mock transport covering timeout, disconnect, duplicate, refusal, error | done |
| Realtime WebSocket transport | done; real-account acceptance pending |
| Web control-centre mode selector and status panel | done |
| Spoken `stop` cancels local playback and closes the Live session | done |
| Home Assistant `homeassistant.conversation` tool | not done; lights and HA-owned media fail closed |
| LED patterns for Live states | not done |
| Process-level mode orchestration (stopping `sttd`/`ttsd`/`wyomingd`) | not done |

`lived` starts as an available but disarmed service. The control-centre selector
uses `/api/v1/live` to arm or disarm its wake subscription and disarms the
currently selected local assistant first. A reboot returns GPT-Live to the safe,
disarmed state; no idle microphone audio is transmitted.

The first device-test MVP deliberately limits local actions to the allow-list
below. General lights and named music requests need the future Home Assistant
conversation bridge and are refused rather than acknowledged without execution.

## Delegation

Tools are matched against a literal table in `live_tools.c`. Anything else fails
closed with a sentence the model can speak, because a refused tool must not end
the conversation.

```
timer.set  timer.cancel  timer.dismiss  timer.query
media.stop media.status  radio.play
device.time  device.volume  device.weather
voice.request (bounded natural-language router for the actions above)
```

Every handler is a client of the daemon that already owns the capability, so
behaviour cannot drift between the local assistant and a Live conversation.
Arguments are validated before a daemon sees them: no paths, no shell
metacharacters, no out-of-range durations, no non-HTTP radio URLs.

Each delegation carries an id and the result is remembered in a bounded cache.
A transport that reconnects and replays a request gets the stored answer and the
action does **not** run twice — which is the difference between a duplicated
timer and a duplicated door unlock.

## Measurements on the MT8163 Echo

Taken with the staged daemon driving the real `waked` stream on the device:

| Measurement | Value |
|---|---|
| Ambient post-AEC level (idle) | ~520 RMS floor, 2,600 RMS peak |
| Preroll window filled from the live stream | 48,000 samples (3.0 s) |
| Input forwarded per conversation | 5.43 s over one session |
| First model audio after wake | 115 ms |
| Model audio delivered to `system.pcm` | 7,680 stereo frames (160 ms), 0 write errors |
| Session outcome | `completed`, reason `timeout`, 0 failures |
| Delegation to live `audiod` (`device.volume`) | 1 dispatch, 0 failures, result returned |
| Duplicated delegation id | 1 dispatch, 1 deduped, 2 results returned |
| Spurious barge-ins at ambient level | 0 (with the relative threshold) |

The barge-in threshold is the one value that cannot be chosen from source: an
initial absolute default of 900 RMS tripped on this device's own ambient audio,
which sits near 3,000. It is now
`max(barge_in_rms, ambient_floor * barge_in_factor)`, with the ambient floor
tracked with asymmetric smoothing (fast down, slow up) so a barge-in can never
raise the threshold that is about to detect it. `input_floor_rms`,
`input_peak_rms` and `speech_peak_rms` are all reported in the status JSON so
the value can be re-measured on any device instead of guessed.

## Operating it

```sh
# Status and metrics
libreecho-lived --socket /run/libreecho/live.sock        # speaks {"cmd":"status"}
                                                         # also: transcript, tools,
                                                         # set_enabled, wake, stop, set_mock

# Fail-closed check (the real transport, no account)
libreecho-lived --foreground --transport realtime

# Full pipeline against the mock transport, on a device
libreecho-lived --foreground --transport mock --mock-scenario session \
    --socket /run/libreecho/live.sock --wake-socket /run/libreecho/wakeword.sock \
    --audio-bus /run/libreecho-audio/system.pcm

# Synthesize a wake without saying the word
#   {"v":1,"id":1,"cmd":"test","args":{}}   on /run/libreecho/wakeword.sock
```

`make test-lived` runs the host suite: preroll ring indexing, the session state
machine against every mock scenario, the delegation allow-list, the playback
path (bus format, missing bus, stalled bus, refused rates, barge-in reset) and
the whole daemon end-to-end against a stand-in `waked`.

## Privacy

Selecting GPT-Live means post-AEC speech audio leaves the device for the
duration of an active conversation. The distinction the control centre must
present:

| Mode | What leaves the device |
|---|---|
| Local LibreEcho | Recognised text only, to the configured model provider |
| Home Assistant | Post-AEC audio, to the configured Home Assistant server |
| GPT-Live | Post-AEC audio, including up to 150 ms of RAM-only preroll, after the wake word starts a conversation |

Idle microphone audio is never transmitted. The preroll buffer is RAM-only,
bounded at 96 KB, never persisted, and discarded after use. The transcript is
bounded at ten turns and is not written to disk. Credentials stay in
`/data/libreecho/secrets/` at mode 0600 and are never logged or published
through the status socket.

Hardware retest with model output measured AEC residual at 3353 RMS over a 569 RMS floor; the default relative factor is 6, putting the threshold at 3414 RMS. The prior factor of 3 produced one false barge-in in that run.
