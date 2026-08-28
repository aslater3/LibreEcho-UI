# LibreEcho-UI API Reference

Complete reference for the `/api/v1/` HTTP API.

## Authentication

| Header | Purpose |
|--------|---------|
| `Authorization: Bearer <token>` | Required for LAN access (if enabled) |
| `X-LibreEcho-CSRF: <token from GET /api/v1/config>` | Required for all state-changing requests; generated per daemon boot |
| `X-LibreEcho-Confirm: confirm-device-action` | Required for destructive actions |
| `Origin: http://<host>` | Required for CORS (if `--allowed-origin` set) |

When a private users file is supplied with `--users-file`, use `POST
/api/v1/auth/login` with a username and password to receive a short-lived
in-memory bearer session. `GET /api/v1/auth` checks the current session and
`POST /api/v1/auth/logout` invalidates it. User records contain only a username,
salt and SHA-256 password digest; the users file is never returned by the API.
Failed user logins are rate limited after five attempts in a 60-second window.

When the users file is absent, the API reports `authentication: bootstrap-required`
and the root page becomes the first-run account setup page. `POST
/api/v1/auth/bootstrap` accepts `username`, `password`, and `password_confirm` and
creates the first account atomically. The endpoint is one-shot: once it succeeds,
normal authentication is immediately required and the response includes a session
token for the new account.

Authenticated administrators can manage local accounts with `GET /api/v1/auth/users`,
`POST /api/v1/auth/users`, and `DELETE /api/v1/auth/users/{username}`. The last
remaining user cannot be removed. Passwords are never returned by these endpoints.

The development image may explicitly use `--allow-insecure-lan`, but that only
relaxes the Origin check. It does not disable configured user or bearer-token
authentication.

## Response Format

All responses use the same envelope:

```json
{
  "ok": true|false,
  "data": { ... },
  "error": {
    "code": "not_supported|invalid|io|busy|auth|update_rejected",
    "reason": "Optional sanitized machine-readable reason",
    "message": "Human-readable description"
  }
}
```

## Endpoints

For `POST /api/v1/system/update/upload`, a rejected installer response may include
`error.reason`, a bounded token containing only lowercase letters, digits, `_`, and
`-`. For example, `current_slot_not_confirmed` identifies the installer check
that refused the package; the field is omitted when the helper emits no `ERROR:`
token. `manifest_update_channel_mismatch` also receives a channel-specific human
message.

### System Status

#### GET /api/v1/status

Returns system health and telemetry.

**Response:**
```json
{
  "ok": true,
  "data": {
    "backend": "linux",
    "simulated": false,
    "uptime_seconds": 3600,
    "cpu_percent": 15,
    "cpus": {
      "count": 4,
      "cores": [
        { "id": 0, "online": true, "utilization_percent": 12, "frequency_khz": 1300000 }
      ]
    },
    "memory_percent": 45,
    "memory_used_mb": 450,
    "memory_total_mb": 1000,
    "storage_percent": 30,
    "storage_used_mb": 2400,
    "storage_total_mb": 8000,
    "storage_available": true,
    "storage_state": "filesystem",
    "temperature_c": 42,
    "device_state": "online"
  },
  "error": null
}
```

When the recovery image has only its in-memory root filesystem mounted, the
API reports `storage_available: false`, `storage_percent: null`, and
`storage_used_mb: null`. It may still report the raw eMMC capacity in
`storage_total_mb` with `storage_state: "block-device-unmounted"`; this is
capacity information, not a claim about used space.

#### GET /api/v1/device

Returns device information.

**Response:**
```json
{
  "ok": true,
  "data": {
    "name": "LibreEcho",
    "hostname": "libreecho",
    "model": "LibreEcho device",
    "serial": "unavailable",
    "os_version": "LibreEcho OS",
    "kernel": "6.1.x",
    "hardware_revision": "adapter pending",
    "backend": "linux",
    "audio": {
      "capture": {
        "rate_hz": 16000,
        "raw_channels": 9,
        "microphones": 7,
        "transport_channels": 2,
        "format": "pcm_s24_3le",
        "valid_bits": 16,
        "beamforming": "measured delay-and-sum on logical mics 0 and 3",
        "high_pass_hz": 80,
        "digital_gain": "4.0x",
        "response": "flat within 0.5 dB, 200 Hz to 7 kHz",
        "noise_floor_dbfs": -65.3,
        "thd_n_percent_max": 0.2,
        "clipping_from_input_amplitude": 16000
      },
      "output": {
        "rate_hz": 48000,
        "channels": 2,
        "format": "pcm_s16_le",
        "mixer_volume_range": "0-175, unity at 127",
        "buses": ["media", "system", "announcement", "alarm"]
      },
      "streaming": {
        "decoders": [],
        "available": ["airplay2", "bluetooth-a2dp"],
        "note": "no compressed-audio decoder on this image; AirPlay 2 and Bluetooth A2DP provide streaming playback"
      }
    }
  },
  "error": null
}
```

`audio` describes what the hardware and this image can do; it is reference
information and none of it is settable here. Every member is optional: an older
daemon omits `audio` entirely and the mock backend does not synthesize one, so
clients must render what is present rather than assume a field exists.

`streaming.decoders` being empty is the meaningful case today — the device has
no compressed-audio decoder, so it cannot play a stream URL itself.
`streaming.available` lists how audio does reach it: already decoded, over
AirPlay 2 and Bluetooth A2DP.

On supported devices, the first-boot hostname is platform-defined and can be
changed by the user. A selected hostname is stored in
`/data/libreecho/config/web-config.json` and takes precedence on later boots.
Device-specific identifiers are not included in public API examples.

### Audio

#### GET /api/v1/playback

Returns the unit's unified playback state. `state` is one of `idle`, `playing`,
`system`, `announcing`, or `alarm`; priority buses can overlap. `source`
identifies the active integration when known. Track metadata is nullable
because not every sender or future integration supplies it.

```json
{
  "ok": true,
  "data": {
    "state": "playing",
    "source": "radio",
    "buses": {
      "media": true,
      "system": false,
      "announcement": false,
      "alarm": false
    },
    "metadata": {
      "available": true,
      "title": "Artist - Track title",
      "artist": null,
      "album": null,
      "station": "Groove Salad"
    },
    "transport": {
      "play": false,
      "pause": false,
      "stop": true,
      "reason": "A live radio stream can be stopped but not paused; starting it again rejoins the broadcast where it is now."
    }
  },
  "error": null
}
```

`source` is `radio` while radiod is streaming, and that answer comes from
radiod rather than from the audio engine's status file: radiod writes straight
onto the media bus, so it reporting a live player is a fact even when the
engine that writes the status file is not running.

`metadata.station` is the station's display name — the `name` from the stored
station list when the playing URL matches one, otherwise the `icy-name` the
stream sent about itself, and `null` when neither exists.

`metadata.title` for radio is the ICY `StreamTitle` the station interleaves
into the audio. It is `null` when the station sends no metadata at all, which
many do not: no title is derived from the URL or the station name to fill the
gap. `artist` and `album` are always `null` for radio — `StreamTitle` is one
free-text field, usually but not reliably `Artist - Title`, and splitting it on
a hyphen would invent structure the station never sent.

`transport` says what this device can actually do to whatever is playing, so a
client can disable a control with a reason instead of offering one that fails:

| Field | Meaning |
| --- | --- |
| `play` | `POST /api/v1/playback/transport` with `play` will start something |
| `pause` | always `false`; see below |
| `stop` | there is something playing that this device can stop |
| `reason` | one sentence explaining the limit, suitable for display |

`pause` is always `false`, and this is a property of the device rather than a
gap in the implementation. Internet radio is a live stream with no buffered
position to resume from, so radiod can stop and reconnect but cannot pause.
AirPlay and Bluetooth are the other way round: the phone is the controller and
LibreEcho is the speaker. `airplayd` exposes status and an enable switch only,
and the Bluetooth stack here is an AVRCP *target*, which answers transport
commands and has no path to send them.

#### POST /api/v1/playback/transport

Starts or stops what is playing. Requires `X-LibreEcho-CSRF`.

**Request:**
```json
{ "action": "stop" }
```

`stop` stops internet radio. `play` starts the last station started in this
session again; that is a fresh connection which rejoins the broadcast live, not
a resume. The remembered station is held in memory only and is forgotten on
restart, because after a reboot nothing was interrupted and offering to resume
something would be a guess.

`pause` is accepted and answered `501` with the reason, rather than rejected as
an unknown action: a client that asks deserves to be told why the device cannot
do it.

| Status | Meaning |
| --- | --- |
| `400` | `action` was missing or was not `play`, `pause` or `stop` |
| `405` | method other than `POST` |
| `409` | nothing to stop, nothing to start again, or a stream is already playing |
| `501` | `pause`, or this image has no stream player |
| `503` | the command reached the player and failed |

The successful response is the same document as `GET /api/v1/playback`, read
after the action, so a client can re-render from it without a second request.

This is the only transport in the product. `agentd` has no radio path, so
"stop" spoken over the music does not reach radiod, and the physical buttons
do not either: `buttond` handles volume up, volume down and microphone mute
only, so the `Play / pause` short-press action stored by `PUT /api/v1/buttons`
is a saved preference that nothing acts on yet.

#### GET /api/v1/audio

Returns current audio state. When the audio adapter is absent, this remains a
successful `200` response with `data.available: false` and
`data.unavailable: true`; the browser uses that explicit capability result to
render the existing unsupported state without logging an HTTP error.

**Response:**
```json
{
  "ok": true,
  "data": {
    "volume": 50,
    "microphone_gain": 65,
    "notification_volume": 70,
    "microphone_muted": false,
    "startup_sound": true,
    "amplifier_on": true,
    "output_available": true,
    "tts_voice": "southern-female",
    "tts_voices": [
      { "id": "southern-female", "name": "Southern English — female" },
      { "id": "northern-male", "name": "Northern English — male" }
    ]
  },
  "error": null
}
```

#### PUT /api/v1/audio

Update audio settings.

**Request:**
```json
{
  "volume": 75,
  "microphone_gain": 80,
  "microphone_muted": false,
  "tts_voice": "southern-female"
}
```

**Response:** Updated audio state (same as GET).

#### POST /api/v1/audio/test

Play test tone.

**Response:**
```json
{
  "ok": true,
  "data": { "playing": true },
  "error": null
}
```

#### POST /api/v1/audio/sample

Play one bundled raw sound by name for previewing the action-button rotation.
The request is state-changing and requires `X-LibreEcho-CSRF`.

**Request:**
```json
{ "name": "action-1" }
```

Names must be 1–48 characters and contain only lowercase letters, digits,
hyphens, or underscores. The mock backend returns `501` because it has no
speaker; a target without `audiod` does the same.

**Response:**
```json
{
  "ok": true,
  "data": { "playing": true },
  "error": null
}
```

Other methods return `405`.

#### POST /api/v1/audio/announce

Speak text through the local TTS service and `audiod` announcement bus. This
is the supported path for external local services; they do not need direct
access to the model, PCM FIFO, ALSA device, or amplifier.

**Request:**
```json
{
  "text": "Now playing Don’t Look Back in Anger by Oasis"
}
```

**Response:**
```json
{
  "ok": true,
  "data": { "speaking": true },
  "error": null
}
```

Use `POST /api/v1/audio/announce/stop` with `{}` to interrupt the active
announcement. State-changing API calls require the normal CSRF header and,
when configured, local API authentication.

### Voice assistant

The voice assistant uses local wake-word detection, post-AEC microphone audio,
local streaming speech recognition, a subscription-authenticated response
provider, and the same local announcement bus as the announce API. It never
accepts or falls back to an OpenAI API key.

#### GET /api/v1/voice-pipeline

Returns speech-pipeline configuration and endpoint health. The response also
includes the persisted `listening` object:

```json
{
  "max_utterance_ms": 6000,
  "end_silence_ms": 1500,
  "vad_floor_rms": 45
}
```

`max_utterance_ms` accepts 2000–20000 milliseconds, `end_silence_ms` accepts
200–3000 milliseconds, and `vad_floor_rms` accepts 1–1024. These values are
read from the same persisted `web-config.json` used by `libreecho-sttd`; they
are not inferred from the web daemon's environment.

#### PUT /api/v1/voice-pipeline

Updates the pipeline mode and optional Wyoming settings. The listening fields
are writable here and are validated before any setting is committed:

```json
{
  "mode": "local",
  "max_utterance_ms": 6000,
  "end_silence_ms": 1500,
  "vad_floor_rms": 45
}
```

Malformed or out-of-range listening fields return HTTP 400 and leave the
previous configuration unchanged. The request requires `X-LibreEcho-CSRF`.

#### GET /api/v1/assistant

Returns assistant configuration, ChatGPT device-login state, pipeline
connectivity, local STT timing, and first-audio latency telemetry. The latency
measurement is from the estimated end of speech to the first PCM submitted to
the announcement bus; the current target is 3000 ms.

#### GET /api/v1/assistant/history

Returns the newest bounded turn records measured by `agentd` itself:

```json
{
  "history_generation": 7,
  "turns": [
    {
      "at_ms": 1724457600123,
      "stt_audio_ms": 1200,
      "stt_processing_ms": 2800,
      "stt_total_ms": 4000,
      "first_text_ms": 2500,
      "first_announce_ms": 3000,
      "first_pcm_ms": 3100,
      "follow_up": false
    }
  ]
}
```

The ring retains up to 12 newest turns in `agentd` memory and is empty after
that daemon restarts. `history_generation` is persisted on the device and
increments on every device-wide clear; clients must discard cached rows when
the generation changes, but may preserve them when an empty response has the
same generation (for example after an agentd restart). Clients should keep
their last non-empty cached result when a successful response contains no
turns. `at_ms` is Unix epoch milliseconds; latency fields are device-local
durations. Other verbs return 405.

#### POST /api/v1/assistant/history/clear

Clears the device-side history ring. This is a state-changing request and
requires `X-LibreEcho-CSRF`; the response is an empty success object. The
browser clears its cache only after this device operation succeeds.

#### PUT /api/v1/assistant

Updates the provider-neutral assistant configuration:

```json
{
  "enabled": true,
  "provider": "openai-codex",
  "model": "gpt-5.4",
  "prompt": "Reply in concise, natural spoken English without markdown."
}
```

The prompt is sent as the response provider's instruction text. Keep it
voice-safe: concise prose, no markdown, URLs, citations, emoji, or claims that
an external action succeeded without tool confirmation.

#### POST /api/v1/assistant/auth/start

Starts ChatGPT subscription device login. The response contains a
`verification_url` and `user_code`; no account password or API key is entered
on LibreEcho.

#### POST /api/v1/assistant/auth/poll

Checks whether device login has completed. Respect the `auth_state` and
server-derived polling interval exposed by assistant status. Successful OAuth
credentials are written with mode `0600` below
`/data/libreecho/secrets/` and are excluded from configuration exports,
diagnostics, image builds, and logs.

#### POST /api/v1/assistant/logout

Deletes the persistent ChatGPT OAuth credentials from the device.

#### POST /api/v1/assistant/respond

Streams a text request through the configured provider and speaks response
segments as soon as a voice-safe boundary is available:

```json
{
  "text": "What is the weather likely to be like today?"
}
```

This endpoint is primarily a setup and diagnostics path. Normal operation is:
wake event, continuous post-AEC audio, local streaming STT, streamed response
text, warm local TTS, then `audiod` announcement playback.

#### POST /api/v1/audio/noise

Starts the built-in sleep-noise generator. `colour` is `white`, `pink` or
`brown`; `level` is 1-100; `minutes` is a sleep timer, 0 to play until stopped.
Synthesis is on-device, so it keeps playing with no network.

```json
{ "colour": "brown", "level": 40, "minutes": 30 }
```

The generator is reported by `GET /api/v1/audio` under `noise`, where
`remaining_seconds` is -1 when running untimed. A timed generator stops on its
own and the state clears without a further request.

#### DELETE /api/v1/audio/noise

Stops the generator.

#### POST /api/v1/audio/simulate

Renders `text` with the device's own text-to-speech and plays it into the
**microphone** path, so wake-word detection, speech-to-text and the assistant
process it exactly as though it had been spoken in the room. Intended for
testing; nothing is recorded and nothing leaves the device.

```json
{ "text": "Alexa, what time is it?" }
```

The audio is substituted for the microphones without interrupting the capture
stream, so no daemon is restarted and wake detection continues throughout. If
the capture mux is not present on the image, the request fails with 501 and the
microphones are unaffected.

### LED

#### GET /api/v1/led

Returns LED ring state. When `ledd` is absent, this remains a successful `200`
response with `data.available: false` and `data.unavailable: true`; mutating LED
requests retain their normal unavailable-adapter error behavior.

**Response:**
```json
{
  "ok": true,
  "data": {
    "colour": { "r": 72, "g": 216, "b": 118 },
    "brightness": 70,
    "visualizer_enabled": true,
    "visualizer_active": false,
    "visualizer_owner": "",
    "visualizer_mood": "idle",
    "visualizer_levels": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    "pixels": [
      { "r": 50, "g": 151, "b": 83 }
    ],
    "boot_profile": { "r": 72, "g": 216, "b": 118, "brightness": 70 },
    "profiles": {
      "listening": "#48b9ff",
      "thinking": "#a873ef",
      "error": "#ef5050",
      "dnd": "#be2323"
    }
  },
  "error": null
}
```

`visualizer_enabled` is the persistent user preference. When false, incoming
music spectrum frames are ignored and the ring displays the underlying
pattern, animation or steady state. `pixels` contains 12 entries on the
physical IS31FL3236 ring. During an enabled audio-reactive frame these are the
brightness-scaled colours currently sent to the hardware.

#### PUT /api/v1/led

Update LED settings.

**Request:**
```json
{
  "r": 255,
  "g": 0,
  "b": 0,
  "brightness": 80,
  "visualizer_enabled": true
}
```

#### PUT /api/v1/led/profile

Save one named LED profile. The profile name must be `listening`, `thinking`,
`error`, or `dnd`; colour channels are 0–255 and brightness is optional from
0–100 (default 100).

**Request:**
```json
{
  "name": "listening",
  "r": 72,
  "g": 216,
  "b": 118,
  "brightness": 80
}
```

**Response:** The updated LED state using the standard success envelope. Invalid
names, colours, or brightness values return `400`; unavailable hardware returns
`501`. The request requires `X-LibreEcho-CSRF`.

#### POST /api/v1/led/test

Run LED test pattern.

**Response:**
```json
{
  "ok": true,
  "data": { "testing": true },
  "error": null
}
```

#### PUT /api/v1/led/night

Configures night mode, which caps the ring to a dim theme on a local-time
schedule. `start_minute` and `end_minute` are minutes past local midnight, so a
window may wrap midnight (for example 1320 to 420 is 22:00-07:00).

```json
{ "enabled": true, "start_minute": 1320, "end_minute": 420,
  "r": 255, "g": 40, "b": 0, "brightness": 10 }
```

The schedule is evaluated in **local** time, so the device timezone must be
correct or the window fires at the wrong hour. See
`PUT /api/v1/system/timezone`. Current state is reported by `GET /api/v1/led`
under `night`, including whether the cap is currently `active`.

### Network

#### GET /api/v1/network

Returns Wi-Fi association state plus an independent gateway-liveness result. A
`connected` association is not considered healthy until the gateway probe has
succeeded.

**Response:**
```json
{
  "ok": true,
  "data": {
    "state": "connected",
    "connectivity": "healthy",
    "recovery_stage": "none",
    "gateway_reachable": true,
    "liveness_failures": 0,
    "ssid": "MyNetwork",
    "signal": 75,
    "rssi_dbm": -47,
    "ip": "192.0.2.10",
    "gateway": "192.0.2.1",
    "dns": "8.8.8.8, 8.8.4.4",
    "hostname": "libreecho",
    "wifi_mac": "",
    "wifi_mac_factory": "",
    "wifi_mac_configured": "",
    "bt_mac": "",
    "bt_mac_factory": "",
    "bt_mac_configured": "",
    "internet": true,
    "dhcp": true,
    "ssh": false,
    "api_lan": false,
    "api_lan_effective": false,
    "api_lan_forced": false
  },
  "error": null
}
```

`connectivity` is one of `unknown`, `healthy`, `degraded`, `recovering`, or
`disconnected`. `gateway_reachable` is `null` until a probe can be completed.
While no default route exists, no probe can run: before this boot has observed
a healthy gateway the liveness result stays `unknown`, and once a healthy
probe has been observed a lost route counts as a failed liveness result.
After this boot has observed a healthy gateway, three consecutive failures arm
bounded recovery: wpa reassociation, then a one-second interface down/up cycle.
If the gateway is still unreachable after both grace periods, `networkd` tries
to atomically reserve `/data/libreecho/network-recovery-reboot.guard` before
submitting `/tmp/reboot.request` to the initramfs reboot supervisor. A successful
submission reports `recovery_stage: reboot-requested`. The persistent guard is
not cleared automatically: if recovery exhausts again after a later daemon or
device restart, `networkd` reports `recovery_stage: exhausted` and does not
request another recovery reboot until an operator has diagnosed the failure and
explicitly removes the guard. A failed or colliding request also reports
`exhausted`; liveness probes continue so a later healthy reply clears the
recovery state without reissuing the failed reboot request. An existing request
is accepted only when its exact content is `reboot`. Each recovery action is
attempted at most once per cycle. The daemon never accesses `/dev/wmtWifi`
directly, preserving the one-radio-transition-per-boot rule.

Liveness probes use a raw ICMP echo on the Wi-Fi interface.
`SO_BINDTODEVICE` is attempted to scope the probe to that interface, but the
MT8163 kernel reports `ENOPROTOOPT` for that option on raw ICMP sockets; that
specific failure is treated as advisory and probing continues. Reply matching
still validates the gateway source address, ICMP identifier, sequence number,
and both header checksums, so an unmatched reply cannot mark the gateway
healthy. Any other bind failure remains fatal and reports the probe as
unavailable.

#### PUT /api/v1/network

`api_lan` is the persisted setting. `api_lan_effective` is the access state
that is actually in force. `api_lan_forced` is true when the daemon was
started with the development-only `--allow-insecure-lan` override, in which
case the effective access remains enabled even if the persisted setting is
false.

**Request:**
```json
{
  "hostname": "myecho",
  "ssh": false,
  "api_lan": true,
  "wifi_mac": "<six hexadecimal octets>",
  "bt_mac": "<six hexadecimal octets>"
}
```

`wifi_mac` and `bt_mac` are optional persistent overrides. Each must contain
six hexadecimal octets separated by colons or hyphens and must be a unicast
address; send an empty string to clear an override. The values are applied by
the web daemon during the next Linux boot: Wi-Fi uses the platform `ip` tool,
and Bluetooth uses the platform `btmgmt` public-address command. If a target
lacks the required helper or controller operation, startup records a bounded
warning while leaving the configured value visible for diagnosis.

The response fields `wifi_mac` and `bt_mac` report live interface values;
`*_factory` reports board identity values when available; and `*_configured`
reports persisted overrides. Empty strings mean that a value is unavailable or
unset.

#### GET /api/v1/network/wifi/scan

Scan for WiFi networks.

**Response:**
```json
{
  "ok": true,
  "data": {
    "networks": [
      { "ssid": "MyNetwork", "security": "wpa2", "signal": 75 },
      { "ssid": "OtherNetwork", "security": "wpa2", "signal": 45 }
    ]
  },
  "error": null
}
```

#### POST /api/v1/network/wifi/connect

Connect to a WiFi network. The `security` field accepts exactly `open`, `wpa2`,
or `wpa3`; if omitted, it defaults to `wpa2` for backward compatibility.
Malformed or unsupported security values are rejected with HTTP 400 before any
adapter request is made.

**Request:**
```json
{
  "ssid": "MyNetwork",
  "password": "secret123",
  "security": "wpa2"
}
```

For an open network, use `"security": "open"` and omit `password`. WPA3 uses
`"security": "wpa3"`. The endpoint never silently converts an invalid security
value to an open or WPA2 network.

**Response:**
```json
{
  "ok": true,
  "data": { "state": "connecting" },
  "error": null
}
```

Invalid security values return the standard error envelope with HTTP 400.

#### POST /api/v1/network/wifi/disconnect

Disconnect from WiFi.

**Response:**
```json
{
  "ok": true,
  "data": { "state": "disconnected" },
  "error": null
}
```

### System Control

#### GET /api/v1/system

Returns system information, NTP synchronization, PMIC RTC persistence, and OTA
state. The hardware clock is maintained in UTC.

**Response:**
```json
{
  "ok": true,
  "data": {
    "update_channel": "stable",
    "ota": {
      "supported": false,
      "design": "A/B",
      "current_slot": "A",
      "inactive_slot": "B",
      "state": "idle",
      "progress": 0,
      "rollback_available": false
    },
    "timezone": "UTC",
    "ntp": true,
    "ntp_state": "synchronized",
    "ntp_servers": "time.cloudflare.com,time.nist.gov,ntp1.npl.co.uk,ntp2.npl.co.uk",
    "last_sync_epoch": 1784927059,
    "clock_valid": true,
    "clock_source": "ntp",
    "rtc_available": true,
    "rtc_persisted": true
  },
  "error": null
}
```

#### POST /api/v1/system/reboot

Reboot device. Requires `X-LibreEcho-Confirm: confirm-device-action`.

**Response:**
```json
{
  "ok": true,
  "data": { "accepted": true },
  "error": null
}
```

#### POST /api/v1/system/shutdown

Shutdown device. Requires confirmation.

#### POST /api/v1/system/factory-reset

Factory reset device. Requires confirmation.

#### PUT /api/v1/system/update/channel

Select the signed GitHub Releases channel. Changing the channel clears the
previous check result; run the update check again before installing.

```json
{"channel":"stable"}
```

`channel` must be `stable` or `dev`.

#### POST /api/v1/system/update/upload

Upload a manually selected OTA tar to the inactive slot. The upload is
signature-verified by default and requires `X-LibreEcho-CSRF`.

`GET /api/v1/system/update` reports two sizes for this endpoint:

| Field | Meaning |
| --- | --- |
| `max_upload_ceiling_bytes` | The fixed ceiling, 33554432 (32 MiB). A body larger than this is always refused. |
| `max_upload_bytes` | What this device will actually accept: the ceiling, capped by the free space on the filesystem the package is staged to, less a margin for the installer. Never greater than the ceiling. |

Read `max_upload_bytes` and show it before a file is chosen. The package is
streamed to a file under `/data/libreecho/update/incoming/` before the installer
is handed it, so a device with less free space there than the ceiling cannot
take a package of that size no matter what the ceiling says. Both limits are
checked before the body is read: a body over the ceiling answers `413` with
`reason: update_size` and the generic message, and a body under the ceiling but
over what the device can stage answers `413` naming the device's own limit.
Sending it anyway costs the whole upload before it fails.

The two differing means free space is the binding limit rather than the ceiling,
which is worth telling the user apart — one says the package is too big, the
other says the device needs clearing out.

The device does not report the size of the image already installed in a slot, so
a client can only show the size of the file it is about to send.

`GET /api/v1/system/update` reports `allow_unsigned: true` only when the
installed update helper supports unsigned manual installation. When that flag
is true, clients may send `X-LibreEcho-Allow-Unsigned: 1` for this upload only.
That bypasses signature verification for the selected package; fetched and
automatic updates never honor the header. The UI explicitly warns about this
trust decision before uploading.

#### GET /api/v1/system/timezone

Returns the configured timezone.

#### PUT /api/v1/system/timezone

Sets the timezone as a POSIX TZ string, for example `CST6CDT,M3.2.0,M11.1.0`.
There is no zoneinfo database on the device, so Olson names such as
`America/Chicago` are **not** accepted; busybox honours `TZ` from the
environment and that is what this sets.

```json
{ "timezone": "CST6CDT,M3.2.0,M11.1.0" }
```

Daemons read `TZ` when they start, so the response reports
`"applies": "after restart"`. Anything that is not a plausible TZ string is
rejected with 400 rather than written.

#### GET /api/v1/system/features

Reports the optional feature switches. They are off by default.

**Response:**
```json
{
  "ok": true,
  "data": {
    "simulation": false,
    "usb_host": false,
    "usb_role": "device",
    "usb_role_supported": true
  },
  "error": null
}
```

`usb_host` reports whether the OTG port is currently in host mode, `usb_role` is
the raw role read back from the kernel (`device`, `host` or `none`), and
`usb_role_supported` is `false` on hardware or images with no switchable role.

`simulation` gates `POST /api/v1/audio/simulate`, which plays rendered speech
into the microphone path so wake-word detection, speech-to-text and the
assistant can be exercised without speaking in the room. It is a testing
capability rather than something a live device should offer, so while it is off
the endpoint answers `403` and the web interface hides its Simulation page.

#### PUT /api/v1/system/features

Switches a feature on or off. The value is persisted with the rest of the
configuration, and the response is the new feature state.

```json
{ "simulation": true }
```

`simulation` must be a boolean; anything else is rejected with `400`. Hiding the
menu entry without gating the endpoint would be decoration, so both move
together: the toggle is the only thing that opens `POST /api/v1/audio/simulate`.

`usb_host` moves the OTG port between host mode, where an attached USB drive is
enumerated, and device mode, where the port serves ADB:

```json
{ "usb_host": true }
```

The port cannot do both at once. Unlike `simulation` this is **not persisted**:
`libreecho-init` pins the role back to device on every boot, so a stored setting
can never leave the ADB gadget — this device's only recovery path — switched
off. It is applied immediately and answers `501` when the running kernel exposes
no switchable USB role.

The switch is written to the kernel's `usb_role` class, not to the MUSB `mode`
attribute. Writing `mode` blocks until a USB session that cannot occur while the
port is powered by a host, and takes the ADB gadget down with it; the role
switch is register writes only and returns immediately.

### Configuration

#### GET /api/v1/config

Returns API configuration.

**Response:**
```json
{
  "ok": true,
  "data": {
    "api_version": 1,
    "os_version": "LibreEcho OS 0.14.0",
    "csrf_token": "<64-character per-boot token>",
    "authentication": "development-disabled",
    "bind_policy": "lan-development",
    "max_request_body": 16384
  },
  "error": null
}
```

#### GET /api/v1/config/export

Export the confirmed-safe configuration as JSON. The export remains useful on
Linux when an optional hardware adapter is absent: `partial` is `true` and
the omitted field names are listed in `unsupported`. Passwords, Wi-Fi PSKs,
authentication material, logs and media metadata are never included.

**Response:** The configuration object is returned in the normal API envelope.
The mock backend normally returns `partial: false` and `unsupported: []`.
`hostname_persisted: true` distinguishes a user/config-selected hostname from
the IDME-derived first-boot default.

#### POST /api/v1/config/import

Import configuration from JSON.

**Request:** Raw config JSON (same format as export).

**Response:**
```json
{
  "ok": true,
  "data": { "restored": true, "schema_version": 1 },
  "error": null
}
```

### Logs & Diagnostics

#### GET /api/v1/logs

Returns recent log entries.

**Response:**
```json
{
  "ok": true,
  "data": {
    "entries": [
      {
        "timestamp": 1721476800,
        "boot_seconds": 42,
        "level": "info",
        "message": "LibreEcho web daemon started"
      }
    ],
    "bounded": true,
    "capacity": 128
  },
  "error": null
}
```

#### GET /api/v1/logs/stream

Returns SSE-formatted log stream (one-shot). Each entry includes
`boot_seconds` when produced by the current logging protocol; this monotonic
value remains useful when the device wall clock is not synchronised.

#### GET /api/v1/diagnostics

Returns diagnostic information.

**Response:**
```json
{
  "ok": true,
  "data": {
    "checks": [
      { "name": "configuration", "status": "ok" },
      { "name": "backend", "status": "ok" },
      { "name": "hardware adapters", "status": "development" }
    ]
  },
  "error": null
}
```

#### GET /api/v1/storage/usb

Lists a USB drive attached to the OTG port.

**Response:**
```json
{
  "ok": true,
  "data": {
    "present": true,
    "device": "sda",
    "partition": "sda1",
    "size_bytes": 124006694912,
    "filesystem": "exfat",
    "mounted": true,
    "path": "/run/libreecho/usb",
    "entries": [{ "name": "Music", "directory": true, "size_bytes": 0 }],
    "entry_count": 1
  },
  "error": null
}
```

The port must be in host mode first — `PUT /api/v1/system/features {"usb_host":
true}`. In device mode the kernel enumerates no disk and this answers
`present: false`, which is the same answer as no drive being plugged in.

The mount is read-only (`MS_RDONLY|MS_NOSUID|MS_NODEV`) and nothing in this
endpoint writes to the drive. Bounded like the rest of the API: the first disk,
its first partition, and at most 64 top-level entries.

#### GET /api/v1/diagnostics/kernel

Returns the tail of the kernel ring buffer, most recent last, bounded to the
last 80 records and drained non-blocking from `/dev/kmsg`.

**Response:**
```json
{
  "ok": true,
  "data": {
    "lines": ["usb 1-1: new high-speed USB device number 2 using musb-hdrc"],
    "count": 1
  },
  "error": null
}
```

This exists because switching the USB port to host mode removes the ADB gadget,
which is the only shell this image offers — there is no SSH server. The kernel
log therefore becomes unreadable over USB at precisely the moment it matters,
such as when checking whether an attached drive enumerated. Reading it over the
network closes that gap.

#### POST /api/v1/diagnostics/export

Create and return a bounded structured diagnostic bundle for attachment to a
private or public report. The action requires the normal authenticated API
session (when authentication is configured) and `X-LibreEcho-CSRF`; an empty
JSON object is the request body. The response is ordered consistently and is
capped at 24 KiB of bundle data, so this endpoint does not create a server-side
temporary file or archive.

The bundle includes public-safe product/release identity (including source
commit/digest, running web-service identity, channel, slot/update state and
kernel/UI/platform fields when available), system resource summaries,
reset/pstore summaries, network liveness without SSID/addresses, audio and
voice health, Bluetooth/profile state without remote identifiers, playback
state without media metadata, privacy/button capability state, and up to eight
recent web log entries. Unavailable or malformed subsystem data is represented
as `available: false` or a bounded placeholder and does not abort the export.

**Response:**
```json
{
  "ok": true,
  "data": {
    "schema_version": 1,
    "format": "libreecho-diagnostic-bundle",
    "bounded": true,
    "max_bytes": 24576,
    "summary": "backend=mock;network=healthy;audio=available;bluetooth=disabled",
    "release_identity": {
      "product_version": "LibreEcho OS 0.13.5",
      "channel": "stable",
      "active_slot": "unavailable",
      "pending_slot": "unavailable",
      "ota_state": "idle",
      "source_commit": "public-build-identity",
      "running_service": { "name": "libreecho-web", "version": "LibreEcho OS 0.13.5" }
    },
    "network": { "available": true, "connectivity": "healthy", "gateway_reachable": true },
    "manifest": {
      "redactions": ["wifi_credentials", "ssid_bssid", "ip_addresses", "bluetooth_addresses", "owner_identifiers", "tokens_cookies", "private_paths", "media_metadata"]
    }
  },
  "error": null
}
```

The manifest also lists omitted private keys, OTA signing material, raw pstore
and requester-supplied paths. The browser's **Download diagnostic bundle**
action downloads this response as JSON and offers its short `summary` for
copying into an issue template. No server-side temporary file is created.

### Wake Word

#### GET /api/v1/wake-word

Returns wake word state. When the wake-word service is absent, this remains a
successful `200` response with `data.available: false` and
`data.unavailable: true`.

**Response:**
```json
{
  "ok": true,
  "data": {
    "enabled": true,
    "wake_word": "LibreEcho",
    "sensitivity": 68,
    "cooldown_ms": 2000,
    "model_status": "loaded",
    "detected_count": 5,
    "cpu_cost_percent": 15,
    "memory_cost_mb": 50,
    "local_processing": true
  },
  "error": null
}
```

#### PUT /api/v1/wake-word

Update wake word settings.

**Request:**
```json
{
  "wake_word": "Echo",
  "sensitivity": 75
}
```

#### POST /api/v1/wake-word/test

Trigger wake word test.

**Response:**
```json
{
  "ok": true,
  "data": { "detected": true },
  "error": null
}
```

### Buttons

#### GET /api/v1/buttons

Returns the saved button settings and the live capability state reported by
`libreecho-buttond`. `available` is true only for a fresh connected status
record. `volume_capable`, `hardware_mute`, and `action_capable` reflect the
keys found on the currently discovered evdev devices; `stale` is true when the
status record is missing, disconnected, or older than 15 seconds.
`available_sounds` lists the installed raw sounds that can be previewed, and
`action_sounds` is the comma-separated rotation list in play order. Sound names
are lowercase letters, digits, hyphens, or underscores and are at most 48
characters each; an empty `action_sounds` disables sound playback while keeping
the action-button ring flash.

#### PUT /api/v1/buttons

Update any supported button setting. The request is persisted after every
successful update. `tones` controls the short rising/falling press cues;
`action` controls the separate action-button behavior, and `action_brightness`
and `mute_brightness` are LED-ring brightness values from 0 to 100. The
currently wired action is `sound`; `listen`, `playpause`, and `disabled` are
accepted settings with the corresponding behavior reported by the daemon.
`action_sounds` is a comma-separated list of installed sound names in rotation
order. Each name follows the same 1–48-character lowercase-name rule as the
preview endpoint; an empty string is valid and means no sound is played.
Malformed fields, unsupported actions, and brightness values outside 0–100
return the standard 400 error envelope.

**Request:**
```json
{
  "short_press": "Start listening",
  "long_press": "Open pairing mode",
  "tones": true,
  "action": "sound",
  "action_sounds": "action-1,action-2,action-3",
  "action_brightness": 70,
  "mute_brightness": 60
}
```

**Response:**
```json
{
  "ok": true,
  "data": {
    "short_press": "Start listening",
    "long_press": "Open pairing mode",
    "available": false,
    "state": "stale",
    "volume_capable": false,
    "hardware_mute": false,
    "action_capable": false,
    "stale": true,
    "tones": true,
    "action": "sound",
    "action_sounds": "action-1,action-2,action-3",
    "available_sounds": ["action-1", "action-2", "action-3"],
    "action_brightness": 70,
    "mute_brightness": 60
  },
  "error": null
}
```

Configuration export/import includes `button_action_sounds` alongside the other
button settings. Imports from older exports may omit it; omitted values retain
the current setting, while any present value is validated and restored.

### Bluetooth

#### GET /api/v1/bluetooth

Returns native HCI controller state, Classic/LE capability flags, active
discovery results, stored bond records, and any pending pairing request. When
the Bluetooth adapter is absent, this remains a successful `200` response with
`data.available: false` and `data.unavailable: true`.
The response also includes `profile_state`, `profile_error`, and
`profile_services`. Kernel protocol support is not reported as a userspace
profile: `profile_services` is true only when the corresponding SDP/profile
service is registered and supervised. The current production image reports
`profile_state: "pairing-only"` until an SDP/profile implementation is added.

The response also includes `last_disconnect_reason` and
`last_connect_failed_status`: the most recent MGMT device-disconnected reason
(for example `remote-terminated (0x03)`) and connect-failed status (for
example `connect-failed (0x04)`), retained for pairing/connection triage. Both
are empty strings until the first event of each kind has been observed in this
boot. Connection failures additionally populate `last_error`.

The response also includes `address`, `address_factory`, and
`address_configured` for the controller. `address` is the live HCI address,
`address_factory` is the board-recorded address when available, and
`address_configured` is the persisted override (empty means no override).

#### PUT /api/v1/bluetooth

Update one controller setting:

```json
{ "enabled": true }
```

`discoverable` and `connectable` can also be supplied as booleans. Enabling
the controller is explicit and is limited to one activation attempt per boot.

#### POST /api/v1/bluetooth/pairing-mode

Enter or leave pairing mode:

```json
{ "enabled": true }
```

Entering pairing mode automatically makes the controller connectable,
discoverable, and bondable. It also starts a yellow breathing LED pattern;
leaving it restores the controller settings that were active beforehand.

#### POST /api/v1/bluetooth/scan and POST /api/v1/bluetooth/scan/stop

Start or stop a combined Classic + LE discovery scan. Results are returned by
`GET /api/v1/bluetooth`.

Starting a scan clears the previous discovery results, so `discovered` is empty
in the response to the `POST` itself. The scan then ends by itself when the
controller's inquiry period expires; there is no completion callback, so a
client polls `GET /api/v1/bluetooth` and treats `scanning` returning to `false`
as the end of the scan, with `discovered` then holding what that scan found.
Starting a second scan while one is running discards the results collected so
far. Only devices that are bonded or connected survive into the next scan.

The status snapshot travels in one bounded adapter message. When more devices
are present than fit, `known_devices` is written first and `discovered` is
clipped to what remains, and the clipping is logged; the snapshot is never
dropped. `GET /api/v1/bluetooth` therefore always reports the bonds even in a
crowded radio environment.

Each `discovered` and `known_devices` entry includes `name`, `rssi`, and
`rssi_valid`. Names are refreshed from the remote device's connected EIR data
and persisted with the bond. Connected BR/EDR devices are queried with the
controller's Read RSSI command; `rssi_valid: false` means no current or
persisted measurement is available, not that the signal is 0 dBm. A valid RSSI
is a signed dBm value and may be negative, for example:

```json
{
  "address": "AA:BB:CC:DD:EE:FF",
  "name": "iPhone",
  "type": 0,
  "rssi": -54,
  "rssi_valid": true,
  "paired": true,
  "connected": true
}
```

#### POST /api/v1/bluetooth/pair

Start pairing with a discovered or known device:

```json
{ "address": "AA:BB:CC:DD:EE:FF", "type": 0, "io_capability": 3 }
```

The UI handles pending confirmation, passkey, and PIN requests through
`POST /api/v1/bluetooth/pairing/response`. `POST /api/v1/bluetooth/unpair`
removes the bond and stored controller keys; `POST
/api/v1/bluetooth/disconnect` terminates an active connection.

The LED adapter protocol defines transient `pulse`, `flash`, and
`full_ring_flash` patterns. `half_ring`, `chase`, `dance`, and `random` are
reserved for the per-pixel LED backend; the current full-ring backend rejects
them explicitly rather than pretending to render them.

### Privacy

#### GET /api/v1/privacy

Returns the privacy settings and the effective audio-retention state. Audio
retention is explicit: `none`, `local`, or `remote`. `remote` is only a
configuration contract in this release: the destination uses an HTTPS URL and
credentials are supplied out of band, but the upload transport is not shipped
on the host-verifiable image. Therefore a configured remote target reports
`state: "unavailable"`, `available: false`, `effective_mode: "none"`, and
`fallback: "disabled"`; the device does not silently fill local storage or
pretend that audio was delivered.

The duration is bounded to `24`, `168`, or `720` hours and the size cap is
bounded to `16`, `64`, or `256` MiB. Remote URLs are limited to 255 characters,
require `https://`, and cannot contain embedded credentials, query strings, or
fragments. Configuration exports may include the destination URL, but never
contain remote credentials, API keys, or microphone audio.

**Response (remote configured but unavailable):**
```json
{
  "ok": true,
  "data": {
    "audio_retention": "remote",
    "audio_retention_mode": "remote",
    "audio_retention_hours": 24,
    "audio_retention_max_mb": 64,
    "audio_remote_destination": {
      "configured": true,
      "url": "https://nas.example/retained-audio",
      "transport": "https-post",
      "authentication": "required-out-of-band",
      "credential_state": "not-configured",
      "available": false,
      "state": "unavailable",
      "effective_mode": "none",
      "fallback": "disabled",
      "last_error": "Remote retention transport is unavailable on this release"
    }
  }
}
```

#### PUT /api/v1/privacy

Update privacy settings. The request requires `X-LibreEcho-CSRF` and the normal
same-origin/authentication checks. `audio_retention: "24h"` remains accepted as
a compatibility alias for `local`.

**Request:**
```json
{
  "local_only": true,
  "audio_retention": "remote",
  "audio_retention_hours": 168,
  "audio_retention_max_mb": 64,
  "audio_remote_url": "https://nas.example/retained-audio",
  "diagnostic_telemetry": false,
  "crash_reports": false,
  "log_retention_hours": 24
}
```

Remote mode without a valid HTTPS destination is rejected with `400`. All
mutations persist atomically using the existing `0600` configuration store.

### Integrations

#### PUT /api/v1/integrations

Update integration toggles. The `rest` integration is the canonical LAN REST API access control: its `enabled` value mirrors the effective LAN API state, and its `forced` value is true when development binding keeps access enabled regardless of persisted `api_lan`.

**Request:**
```json
{
  "enabled": true
}
```

(Use query parameter or path to specify integration: `?integration=home-assistant`)

**Response:**
```json
{
  "ok": true,
  "data": {
    "items": [
      { "id": "home-assistant", "name": "Home Assistant", "enabled": true },
      { "id": "mqtt", "name": "MQTT", "enabled": false },
      { "id": "rest", "name": "Local REST API", "enabled": true, "forced": false },
      { "id": "bluetooth", "name": "Bluetooth audio", "enabled": false }
    ]
  },
  "error": null
}
```

#### GET /api/v1/integrations/radio

Returns the stored internet radio stations. A station is asked for by its
`word`; `name` is what is displayed and `url` is the stream.

**Response:**
```json
{
  "ok": true,
  "data": {
    "max_stations": 32,
    "playback_supported": true,
    "playing": true,
    "playing_url": "https://ice1.somafm.com/groovesalad-128-mp3",
    "stations": [
      {
        "word": "groove",
        "name": "Groove Salad",
        "url": "https://ice1.somafm.com/groovesalad-128-mp3",
        "enabled": true
      }
    ]
  },
  "error": null
}
```

`playback_supported` is answered by asking radiod rather than by a constant, so
an image without a stream player reports `false` and a client can say so rather
than implying playback works.

`playing` is radiod's own state and is the authoritative answer to whether a
stream is running; `playing_url` is empty when it is not. The track title and
station name radiod read off the stream are reported by
`GET /api/v1/playback` rather than here, so a client shows now-playing from one
document. A client verifying
that playback stopped should read `playing` rather than a log line. The
amplifier reported by `GET /api/v1/audio` as `amplifier_on` is a useful second
witness — it powers up only when PCM is actually flowing — but it is not a
substitute: it also stays up while the device speaks, so it lags the stream.

#### POST /api/v1/integrations/radio/play

Starts the station whose `word` matches, by its stored URL.

**Request:**
```json
{ "word": "groove" }
```

| Status | Meaning |
| --- | --- |
| `400` | `word` was missing or empty |
| `404` | no station has that word |
| `409` | the station exists but is switched off |
| `501` | this image has no stream player |
| `503` | the station could not be played |

The successful response has the same shape as `GET`, so `playing` and
`playing_url` come back with it.

The assistant does not reach this endpoint. `agentd` has no radio path at all,
so a spoken request for a station cannot start one; playback is started over the
API today.

#### POST /api/v1/integrations/radio/stop

Stops whatever radiod is playing. Stopping a stream that is already stopped is
not an error, so this is safe to send unconditionally as cleanup. Answers `501`
when the image has no stream player and `503` when the stop failed. The
successful response has the same shape as `GET`.

This is the only way to stop the radio today, for the same reason: `agentd` has
no radio path, so "stop" spoken over the music does not reach radiod.

#### PUT /api/v1/integrations/radio

Replaces the whole list. The body is **flat and numbered** rather than an array:
the daemon reads JSON scalars by key and has no array parser, and hand-rolling
one for client-supplied nested data is the sort of code that turns into a buffer
bug. Reads still return a proper array, which is what clients want.

**Request:**
```json
{
  "station_count": 2,
  "station_0_word": "groove",
  "station_0_name": "Groove Salad",
  "station_0_url": "https://ice1.somafm.com/groovesalad-128-mp3",
  "station_0_enabled": true,
  "station_1_word": "drone zone",
  "station_1_name": "Drone Zone",
  "station_1_url": "https://ice1.somafm.com/dronezone-128-mp3",
  "station_1_enabled": false
}
```

For every index `i` below `station_count`, `station_i_word` and `station_i_url`
are required; `station_i_name` and `station_i_enabled` are optional.

| Field | Rule |
| --- | --- |
| `station_count` | 0 to 32 |
| `station_i_word` | 1-31 characters of lowercase letters, digits, spaces or hyphens; no leading or trailing space; unique across the list |
| `station_i_name` | under 64 characters; defaults to the word when blank or absent |
| `station_i_url` | starts `http://` or `https://`, printable ASCII with no spaces, under 512 characters |
| `station_i_enabled` | boolean; defaults to `true` |

Nothing is written unless every station validates, so a rejected request leaves
the stored list untouched. A failure answers `400` with a message describing the
rule that was broken; surface that message rather than a generic error. The
successful response has the same shape as `GET`.

The list is stored beside the other configuration as `radio-stations.json`.

### Events

#### GET /api/v1/events

Returns SSE-formatted event stream (one-shot snapshot).

**Response:** `text/event-stream`
```
id: 1
event: log
data: {"timestamp":1721476800,"boot_seconds":42,"level":"info","message":"started"}

id: 2
event: audio
data: {"changed":true}

event: status
data: {"refresh":true}
```

## Error Codes

| Code | HTTP | Description |
|------|------|-------------|
| `not_supported` | 501 | Hardware daemon not running |
| `invalid` | 400 | Invalid request (bad JSON, missing field) |
| `io` | 503 | I/O error (hardware unavailable) |
| `busy` | 409 | Resource busy (e.g., scan in progress) |
| `auth` | 401/403 | Authentication/authorization failed |

## Rate Limits

- Max 16 concurrent HTTP clients
- Max 16KB request body
- Max 12 WiFi scan results
- Max 128 log entries in memory
- Max 4 adapter clients per daemon
