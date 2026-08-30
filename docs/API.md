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

#### GET /api/v1/setup

Returns first-boot setup defaults and the connectivity prerequisites needed by
the setup page. The response remains available during a degraded Linux boot
when audio, network, or wake-word companion services are unavailable.

`vendor_firmware.state`, `verification`, `source_layout`, and `error` mirror
the bounded boot-time vendor-import status. `force_next_boot` reports whether
the one-shot compatibility marker is pending, and `wlan0_registered` reports
whether the kernel currently exposes the Wi-Fi interface. A degraded wake-word
adapter returns the valid fallback `wake_word: "LibreEcho"`.

```json
{
  "ok": true,
  "data": {
    "wake_word": "LibreEcho",
    "vendor_firmware": {
      "state": "ready",
      "verification": "hash-pinned",
      "source_layout": "etc/firmware",
      "error": "none",
      "force_next_boot": false
    },
    "wlan0_registered": true
  },
  "error": null
}
```

#### POST /api/v1/setup

Validates and applies the first-boot hostname, initial volume, Wi-Fi profile,
wake-word preferences, and privacy choices. Hostname, audio, Wi-Fi, and durable
configuration failures abort the transaction with stage-specific errors.

Wake-word support is optional: if its companion service returns
`LE_NOT_SUPPORTED`, setup continues, the submitted `wake_word` and
`wake_sensitivity` are still written to the canonical configuration, and the
boot-time restore retries them when the service becomes available. Other
wake-word errors abort setup. Wi-Fi credentials are passed to the network
adapter for association but are never returned by the API or written to the
web configuration.

#### POST /api/v1/setup/vendor-import-force-next-boot

Schedules one forced, owner-local firmware import for the next boot. This
endpoint creates only the mode-`0600` one-shot marker; it does not reboot the
device. The import remains structurally checked but is reported as
`forced-unverified`, never hash-pinned.

The request requires normal authentication, `X-LibreEcho-CSRF`, and this exact
confirmation body:

```json
{ "confirm": "force-unverified-owner-local-import" }
```

A successful response reports `force_next_boot: true`,
`reboot_required: true`, and `verification: "forced-unverified"`. An absent or
incorrect confirmation returns `400`; non-Linux backends return `501`; and a
marker write failure returns `503`.

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
    "backend": "linux"
  },
  "error": null
}
```

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
    "source": "airplay2",
    "buses": {
      "media": true,
      "system": false,
      "announcement": false,
      "alarm": false
    },
    "metadata": {
      "available": true,
      "title": "Track title",
      "artist": "Artist",
      "album": "Album"
    }
  },
  "error": null
}
```

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

### Timers and alarms

#### GET /api/v1/timers

Returns the bounded timer schedule. Each entry has an `id`, `kind` (`countdown`
or `alarm`), `state` (`pending` or `ringing`), `seconds_remaining`, and an
optional `label`. The response also includes `ringing`, `missed`, and
`available`. When `timerd` is absent, this GET still returns HTTP 200 with
`available: false`, an empty `timers` array, and zero `ringing`/`missed`
counts. Timer writes return the standard 503 unavailable response instead.

#### POST /api/v1/timers

Creates a countdown and requires `X-LibreEcho-CSRF`.

```json
{ "seconds": 600, "label": "pasta" }
```

`seconds` is required and must be 1–604800. `label` is optional, but if
present must be a valid JSON string whose UTF-8 encoding is no longer than 47
bytes; oversized, malformed, or control-character labels return HTTP 400 rather
than being truncated or changed. Leading whitespace is preserved when the
schedule is persisted and restored. Success returns
HTTP 201 with `{ "id": number }`. The fixed schedule holds at most 16 active
entries; a valid request when it is full returns HTTP 409 with error code
`busy`.

#### POST /api/v1/timers/dismiss

Dismisses all currently ringing timers, leaves pending timers untouched, and
returns `{ "dismissed": number }`. Requires `X-LibreEcho-CSRF`.

#### DELETE /api/v1/timers/{id}

Cancels one pending timer by numeric ID. Ringing timers are not cancelled by
this route; use `/timers/dismiss` to silence them. The entire path component
must be a nonzero decimal integer; malformed, ringing, out-of-range, missing,
or already-cancelled IDs return HTTP 404 without removing another timer.
Requires `X-LibreEcho-CSRF`.

The timer page refreshes its status while open so countdowns and ringing state
remain current. Timer state is persisted atomically with a restrictive
permissions policy and is restored after the wall clock becomes valid.

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
  "api_lan": true
}
```

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

Connect to a WiFi network. The `security` field accepts exactly `open` or `wpa2`;
if omitted, it defaults to `wpa2` for backward compatibility. WPA3/SAE is not
advertised or accepted because the shipped MT8163 path is WEXT-only and has no
verified SAE capability.
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

For an open network, use `"security": "open"` and omit `password`. The endpoint
never silently converts an invalid security value to an open or WPA2 network.

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

### Configuration

#### GET /api/v1/config

Returns API configuration.

**Response:**
```json
{
  "ok": true,
  "data": {
    "api_version": 1,
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

#### PUT /api/v1/buttons

Update button mappings.

**Request:**
```json
{
  "short_press": "Start listening",
  "long_press": "Open pairing mode"
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
    "stale": true
  },
  "error": null
}
```

`available` and the capability fields are derived from the evdev devices
currently discovered by `libreecho-buttond`. A missing, disconnected, or older
than 15-second status record is reported as unavailable/stale; the API never
claims a physical mute/privacy control from a hard-coded default. Physical
press behavior, fail-safe capture muting, and reboot persistence remain
hardware-validation gates.

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
