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
    "code": "not_supported|invalid|io|busy|auth",
    "message": "Human-readable description"
  }
}
```

## Endpoints

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
    "kernel": "3.18.140",
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

Returns current audio state.

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
      { "id": "alan", "name": "Alan — male" }
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

### Voice assistant

The voice assistant uses local wake-word detection, post-AEC microphone audio,
local streaming speech recognition, a subscription-authenticated response
provider, and the same local announcement bus as the announce API. It never
accepts or falls back to an OpenAI API key.

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

### LED

#### GET /api/v1/led

Returns LED ring state.

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

### Network

#### GET /api/v1/network

Returns network state.

**Response:**
```json
{
  "ok": true,
  "data": {
    "state": "connected",
    "ssid": "MyNetwork",
    "signal": 75,
    "rssi_dbm": -47,
    "ip": "192.168.1.100",
    "gateway": "192.168.1.1",
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

Connect to WiFi network.

**Request:**
```json
{
  "ssid": "MyNetwork",
  "password": "secret123",
  "security": "wpa2"
}
```

**Response:**
```json
{
  "ok": true,
  "data": { "state": "connecting" },
  "error": null
}
```

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

Export diagnostic bundle.

**Response:**
```json
{
  "ok": true,
  "data": {
    "filename": "libreecho-diagnostics.json",
    "redacted": true
  },
  "error": null
}
```

### Wake Word

#### GET /api/v1/wake-word

Returns wake word state.

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
    "hardware_mute": true
  },
  "error": null
}
```

### Bluetooth

#### GET /api/v1/bluetooth

Returns native HCI controller state, Classic/LE capability flags, active
discovery results, stored bond records, and any pending pairing request.

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

#### PUT /api/v1/privacy

Update privacy settings.

**Request:**
```json
{
  "local_only": true,
  "audio_retention": "24h",
  "diagnostic_telemetry": false,
  "crash_reports": false,
  "log_retention_hours": 24
}
```

### Integrations

#### PUT /api/v1/integrations

Update integration toggles.

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
      { "id": "rest", "name": "Local REST API", "enabled": true },
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
