# LibreEcho-UI API Reference

Complete reference for the `/api/v1/` HTTP API.

## Authentication

| Header | Purpose |
|--------|---------|
| `Authorization: Bearer <token>` | Required for LAN access (if enabled) |
| `X-LibreEcho-CSRF: libreecho-local` | Required for all state-changing requests |
| `X-LibreEcho-Confirm: confirm-device-action` | Required for destructive actions |
| `Origin: http://<host>` | Required for CORS (if `--allowed-origin` set) |

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
    "temperature_c": 42,
    "device_state": "online"
  },
  "error": null
}
```

#### GET /api/v1/device

Returns device information.

**Response:**
```json
{
  "ok": true,
  "data": {
    "name": "libreecho",
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

### Audio

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
    "output_available": true
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
  "microphone_muted": false
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

#### PUT /api/v1/led

Update LED settings.

**Request:**
```json
{
  "r": 255,
  "g": 0,
  "b": 0,
  "brightness": 80
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
    "api_lan": false
  },
  "error": null
}
```

#### PUT /api/v1/network

Update network settings.

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

Returns system information and OTA state.

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
    "timezone": "Europe/London",
    "ntp": true
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
    "csrf_token": "libreecho-local",
    "authentication": "development-disabled",
    "bind_policy": "loopback-default",
    "max_request_body": 16384
  },
  "error": null
}
```

#### GET /api/v1/config/export

Export complete configuration as JSON.

**Response:** Raw config JSON (not wrapped in envelope).

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

Returns SSE-formatted log stream (one-shot).

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
data: {"timestamp":1721476800,"level":"info","message":"started"}

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
