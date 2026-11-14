# Home Assistant ESPHome voice mode (0.14)

Home Assistant mode keeps LibreEcho's local audio front end and wake detector.
Home Assistant owns the STT, conversation/intent and TTS pipeline after wake:

```text
Echo microphones -> calibration / AEC / beam selection
                 -> local openWakeWord
                 -> ESPHome native API (Noise, TCP 6053)
                 -> Home Assistant Assist
                    -> selected STT / conversation agent / TTS
                 -> local audiod speaker bus
```

The satellite is `libreecho-esphomed`, implemented in C99 for ARM32; it does not
require the Python Linux Voice Assistant runtime. The Wyoming satellite daemon
is removed for 0.14. Wyoming Whisper and Piper **clients** remain supported in
Custom mode, independently of Home Assistant mode.

## Home Assistant setup

Enable **Home Assistant** on the Integrations page. Add the device through Home
Assistant's **ESPHome** integration, using the device's LAN address and TCP port
`6053` if discovery is unavailable. Select an Assist pipeline in Home Assistant.
Local STT, local assistant dispatch and local TTS stay stopped while HA owns
voice. `waked`, `micd`, `audiod`, calibration and AEC remain device-side owners.
Only final post-AEC mono 16-bit PCM at 16 kHz is sent upstream; raw microphone
lanes are not exposed.

If a room is set on the Device page, it is sent as the ESPHome
`suggested_area`, so Home Assistant places the device in that area when it is
first added, and Assist can then target "the lights" in that area. Home
Assistant only applies a suggested area when the device is added; for a device
that is already set up, change its area in Home Assistant.

Besides the Assist satellite, the device offers Home Assistant a media player,
a **Microphone mute** switch and, on boards with the TSL2540 ambient light
sensor, an **Illuminance** sensor (unit `lx`, device class `illuminance`, state
class `measurement`). `libreecho-esphomed` reads the sensor's `als_lux` node at
most once a second and publishes a new value only when it changes by 5% (or
1 lx in the dark), or after a minute for smaller drift, so HA sees no more than
one update a second. A board without the sensor lists no illuminance entity;
if the sensor stops reading, HA shows the entity as unknown. Tests and other
boards can point the daemon at a different file with `--light-sensor PATH`.
The sensor is found when HA lists entities, so a sensor that appears late in
boot shows up at HA's next reconnect.

The last turn is offered as text sensors. **Last wake word** (the wake model
waked reports, on by default) updates when a wake word is heard. **Last
transcript** and **Last reply** carry speech text, so they are listed only when
the owner sets `"esphome_speech_text_sensors": true` in the web configuration;
they are off by default and nothing else is published about speech. The
sensors clear to empty when the voice history is cleared. Values are kept in
memory only, and a change reaches HA only while HA is subscribed.

**Room level** is a dBFS sensor (one decimal, not calibrated to dB SPL) of the
room's sound level, averaged over one-second windows. It is off by default and
listed only when the owner sets `"esphome_room_level_sensor": true` in the web
configuration. It reuses the audio stream the satellite already reads for its
ring, so it opens no second capture. No audio leaves the device for it. It is
published at most every 5 s, and only when the level moves by 2 dB or more, or
once a minute. It reads missing while the microphone is muted or the stream is
stale. The level is taken after the echo canceller, which does not remove the
device's own playback: on bench tests a 2 s test tone at volume 30 raised the
reading by about 25 dB, so playing sound on the device does show in the sensor.

The four hardware buttons are offered as event entities, one each: **Volume up
button**, **Volume down button**, **Microphone mute button** and **Action
button**. Each event's type is the gesture: `press`, `long_press` (reported
once the button has been held for 0.8 s) or `double_press` (two presses starting
within 0.35 s of each other). A short press is reported after that 0.35 s
window, so it can arrive a fraction of a second after the press. The buttons
keep working locally as before; the events only report what the buttons did.
An entity only sees presses from a button the board has.

Device activity is offered the same way, as three more event entities. None of
them carries transcript text, TTS or media URLs, or timer names; each event is
only its type:

- **Voice assistant** (`wake_word`, `listening`, `thinking`, `replying`,
  `finished`, `stopped`): `wake_word` when the wake word is heard on the
  device, `listening` when the request opens, `thinking` when capture ends,
  `replying` when the reply is about to play, and one closing event per turn:
  `finished` when it completed, or `stopped` when it was cancelled or failed.
  Announcements and follow-up turns open and close their own turn the same way.
- **Playback** (`started`, `stopped`): a finite reply or announcement URL starts
  or stops playing on the device. Plain media-player URLs (radio streams) and
  Bluetooth/AirPlay audio are not reported.
- **Timer** (`started`, `updated`, `cancelled`, `finished`): the timer events
  Home Assistant sends for its voice timers. A timer ringing on the device is
  not reported yet.

These entities are event-only: they carry no state, and HA receives them only
while it is subscribed to the device's states, as with the buttons.

The LED ring is offered as an **LED ring** light: on/off, RGB colour, brightness
and three effects, **Solid**, **Pulse** and **Flash** (the ring's own overlay
patterns). HA's setting is shown on the ring only while the device is not
showing its own voice indicator. The voice steps above take the ring while a
turn is open, and HA's state is applied again when the turn ends, so a change
made during a turn waits for it. Turning the light off releases only HA's
overlay; the ring returns to the saved ring settings. The light's state is kept
in the daemon, so a restart of the satellite turns the light off and returns
the ring to its saved settings.

The satellite also offers three **device actions** for Home Assistant automations
(ESPHome user services, under the device's ESPHome services in HA, for example
`esphome.<device>_announce`). Each validates its arguments first; a malformed
call disconnects that Home Assistant client and changes nothing. A call that
arrives during a voice turn, playback or another LED write is ignored rather
than queued, so the device's own activity keeps priority.

- **announce**: `text` (string, 1-200 characters, no control characters). The
  text is spoken through the device's TTS (audiod `speak`, the same path as the
  voice replies). Not available as a URL.
- **play_sound**: `name` (string, lowercase letters, digits, `-` or `_`, up to 48
  characters). Plays one of the bundled sounds by name (currently `action-1`,
  `action-2`, `action-3`); a name with no bundled file plays nothing.
- **set_ring**: `colour` (string `#RRGGBB`), `effect` (string `Solid`, `Pulse` or
  `Flash`) and `duration` (integer, 1-300 seconds). Shows the colour on the ring
  for the duration, then releases only that overlay; the ring returns to what it
  was showing before.

Disabling HA restores the previously selected **Local** or **Custom** mode and
its settings. Custom Whisper/Piper addresses, model/voice choices and assistant
configuration are not erased. There is no protocol selector, legacy satellite
banner, or staged compatibility state. Integration bit 1 remains HA-enabled;
missing/old `ha_protocol` values normalize to `esphome`.

### Cancelling a request

A request can be cancelled at any stage, and the light ring then goes straight
back to idle (no error flash). Home Assistant is told to stop the run, and
nothing from that run is played afterwards:

- **Action button**: while a request is in progress (listening, thinking or
  replying), a press cancels it instead of doing its usual job. A button whose
  action is set to `disabled` stays inert.
- **Home Assistant**: the device exposes a **Cancel request** button entity
  (`button.<device>_cancel_request`), usable from dashboards and automations.
- **Wake word**: saying the wake word again while Home Assistant is thinking
  cancels the request. While listening and replying the wake word does not
  cancel (your own command, or the reply's echo, could contain it); use the
  button, Home Assistant or "stop" there.

For about 1.5 seconds after a cancel, a new wake word is ignored, so late events
from the stopped run can never attach to a new one. `buttond` reaches
`esphomed` through the root-only datagram socket
`/run/libreecho/esphome-control.sock` (`--control-socket`); the only command is
`cancel`, and it does nothing when no request is in progress.

## Server-side wake word (optional, off by default)

By default the wake word is detected on the device by `waked`, and that stays
the only path that starts a turn. The owner can instead let Home Assistant run
wake detection, as in ESPHome's "wake word on Home Assistant" mode, by setting
`"esphome_server_wake": true` in `web-config.json` (no web control yet). The
option is read when the daemon starts or on SIGHUP.

While it is on, Home Assistant owns voice, and the satellite is subscribed,
`esphomed` opens a run with `USE_VAD|USE_WAKE_WORD` and no wake phrase, then
streams the post-AEC mono 16 kHz microphone continuously. Home Assistant's
wake engine decides when a turn begins. On-device wake events are ignored while
the option is on. The ring pulses the listening colour while the stream is open.
`/run/libreecho/esphome-status.json` reports `server_wake` (configured) and
`server_wake_streaming` (audio is being sent now).

Mute, the privacy switch, or the unsubscribe of Home Assistant ends the stream
at once with a stop request. After a run ends or times out, streaming restarts
after one second, unless it is muted or privacy is on. Nothing streams while
the option is off or Home Assistant does not own voice. Saves from the web UI
keep the `esphome_server_wake` key.

## Bluetooth proxy (experimental, off by default)

The satellite can offer Home Assistant a Bluetooth proxy that forwards raw LE
advertisements from `btd`. This is experimental: on current hardware no LE
advertisements have been observed reaching the device, so it is not advertised
unless the owner opts in with `"esphome_bluetooth_proxy": true` in
`web-config.json` (no web control yet). The option is read when the daemon
starts or on SIGHUP, and web saves keep the key.

While it is off (the default) the device info reports
`bluetooth_proxy_feature_flags = 0`, Home Assistant creates no Bluetooth
scanner for the device, a Bluetooth subscribe request is ignored, and
`esphomed` never asks `btd` to scan.

## Protocol contract

The reviewed interoperability baseline is Home Assistant **2026.9.4**, which
pins **aioesphomeapi 46.2.0**, and ESPHome schema commit
`fb65096e`. Native API version is **1.14**. Voice-assistant feature flags are
**61**: the `SPEAKER` bit (2) is deliberately omitted because it selects streamed
TTS PCM instead of URL playback. The connection uses Noise; there is no legacy
AuthRequest gate. Conversation continuation metadata is handled at `INTENT_END`,
not `RUN_END`.

Replies stream. When the pipeline's TTS accepts streamed text, Home Assistant
sends `tts_start_streaming` once the conversation agent has written about 60
characters. The device then opens the reply URL and plays the WAV or MP3 while
it is still being generated. WAV starts once about 150 ms of audio is
buffered. While the device waits for the first audio, or the stream stalls
between sentences, it feeds the speaker silence. This hides the audio output's
power-up and keeps the output from shutting down mid-reply. Media is ducked
from that point, as it is during the reply. A reply fails if no data arrives
for 20 s, or if it runs past 180 s or 32 MiB.

These protocol contracts are host-verification targets, not a claim that this
image has been deployed or acoustically tested. Protocol/framing, audio and
media-event implementation are separate from the Web control-plane checks.

## Private key and status

The canonical configuration stores `esphome_noise_key` as a canonical base64
encoding of exactly 32 bytes (44 characters including one trailing `=`). Empty
is an explicit **unprovisioned** state: the daemon initially uses the all-zero
Noise PSK, and HA provisions the device. Restrict provisioning to a trusted LAN.
Config saves preserve an existing provisioned key. Config and its backups are
mode `0600`; ordinary config exports, voice status and diagnostics never reveal
the key. There is no web reveal/rotation endpoint in this change.

The daemon consumes `--config` through the normal `LE_CONFIG_PATH` and writes
bounded status to `/run/libreecho/esphome-status.json`. Its init script is
`/etc/init.d/libreecho-esphomed.init`, with pidfile
`/var/run/libreecho-esphomed.pid`. Stable factory/persisted device identity is
retained; this migration does not change network or kernel policy.

`GET /api/v1/voice-pipeline` reports `home_assistant.protocol: "esphome"`,
`port: 6053`, `ready`, and `connected`. Readiness binds the trusted daemon
executable, PID/start time/boot identity and its own listening-socket inode to
validated status. Connected additionally requires an actual native API session,
not simply a listening port. Missing/malformed/stale status, an unrelated PID or
an unrelated listener reports false. Audio/capture readiness remains separately
gated by adapter and privacy state; neither field probes dormant Custom
Whisper/Piper endpoints in HA mode.

Native wake selection is persisted independently as `esphome_active_wake_word`
(default Alexa, empty disabled, or `alexa_v0.1`) and survives ordinary config
saves without replacing Local/Custom wake settings. Validated `set_word` reloads
the existing inference worker with a bounded five-second wait; the satellite
allows six seconds for that transaction while ordinary adapter deadlines stay
at 500 ms. Host lifecycle tests script only ONNX compute; trained-model quality
and hardware capture through reload remain separate acceptance gates.

## Transitions and failures

On Linux, a requested transition returns **202/pending** while a tracked worker
stops the old owner and starts the new one. Poll voice status; concurrent voice
changes return **409**, while ordinary APIs continue to operate. A failed
activation restores the prior persisted voice intent before restoring its
service graph, without reverting unrelated settings accepted in the meantime.
The completed failure is visible as `restart.state: "failed"`; a rollback
failure is called out explicitly. Missing ESPHome installation returns **501**
before any local services are stopped. A failed HA stop never activates a
competing local owner.

Focused host checks:

```sh
python3 tests/test_esphome_control.py
python3 tests/test_esphome_control_http.py
node tests/test_voice_assistant_ha_mode_ui.js
```

The HTTP test requires `build/libreecho-web`. Tests use injected private paths
or a private mock configuration, and clean up their fixture processes. They do
not contact hardware or prove speaker/microphone performance.
