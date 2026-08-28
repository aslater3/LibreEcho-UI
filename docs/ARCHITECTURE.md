# LibreEcho-UI Architecture Guide

This document explains how the LibreEcho web management interface works, from the browser down to the hardware.

## System Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              BROWSER                                        │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐ │
│  │   Status    │  │   Network   │  │    Audio    │  │       System        │ │
│  │   Panel     │  │   Panel     │  │   Panel     │  │       Panel         │ │
│  └──────┬──────┘  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘ │
│         │                │                │                    │            │
│         └────────────────┴────────────────┴────────────────────┘            │
│                                    │                                      │
│                              fetch() JSON                                 │
└────────────────────────────────────┼──────────────────────────────────────┘
                                     │
                              HTTP /api/v1/*
                                     │
┌────────────────────────────────────┼──────────────────────────────────────┐
│                         libreecho-web (C99)                              │
│  ┌─────────────────────────────────┼─────────────────────────────────┐   │
│  │              api.c              │                                 │   │
│  │  Routes: /api/v1/status         │                                 │   │
│  │          /api/v1/audio          │                                 │   │
│  │          /api/v1/network        │                                 │   │
│  │          /api/v1/led            │                                 │   │
│  │          /api/v1/system/*       │                                 │   │
│  └─────────────────┬───────────────┘                                 │   │
│                    │                                                 │   │
│  ┌─────────────────▼───────────────┐                                 │   │
│  │         backend_linux.c          │                                 │   │
│  │  ┌──────────┐ ┌──────────────┐  │                                 │   │
│  │  │ Direct   │ │ Adapter      │  │                                 │   │
│  │  │ sysfs/   │ │ Client       │  │                                 │   │
│  │  │ proc     │ │ (le_adapter) │  │                                 │   │
│  │  │          │ │              │  │                                 │   │
│  │  │ /proc/   │ │ AF_UNIX      │  │                                 │   │
│  │  │ uptime   │ │ sockets      │  │                                 │   │
│  │  │ /sys/    │ │              │  │                                 │   │
│  │  │ class/   │ │              │  │                                 │   │
│  │  └──────────┘ └──────┬───────┘  │                                 │   │
│  └──────────────────────┼───────────┘                                 │   │
│                         │                                             │   │
│  ┌──────────────────────┼───────────┐                                 │   │
│  │      log.c           │           │                                 │   │
│  │  le_log_info() ──────┼───────────┼──▶ /run/libreecho/log.sock     │   │
│  └──────────────────────┘           │                                 │   │
│                                     │                                 │   │
│  ┌──────────────────────────────────┘                                 │   │
│  │              config_manager.c                                      │   │
│  │  /etc/libreecho/config.json ◄──▶ SIGHUP reload                   │   │
│  │  /etc/libreecho/history/config-*.json                            │   │
│  └───────────────────────────────────────────────────────────────────┘   │
│                                    │                                     │
│                              AF_UNIX adapter protocol                      │
│         ┌────────────────────────┼────────────────────────┐              │
│         │                        │                        │              │
│         ▼                        ▼                        ▼              │
│  ┌─────────────┐        ┌─────────────┐        ┌─────────────────┐      │
│  │  networkd   │        │   audiod    │        │      ledd       │      │
│  │             │        │             │        │                 │      │
│  │ wpa_ctrl    │        │ ALSA ioctl  │        │ sysfs LED class │      │
│  │ udhcpc      │        │ /dev/snd/*  │        │ /dev/i2c-*      │      │
│  │ netlink     │        │             │        │                 │      │
│  └─────────────┘        └─────────────┘        └─────────────────┘      │
│                                                                            │
│  ┌─────────────────────────────────────────────────────────────────────┐  │
│  │                         libreecho-logd                              │  │
│  │  Receives all logs → /var/log/libreecho/system.log (rotated)        │  │
│  └─────────────────────────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────────────────────────┘
```

## Component Details

### 1. Web Daemon (`libreecho-web`)

**Role:** HTTP server, API routing, config management, orchestration.

**Files:** `src/main.c`, `src/http_server.c`, `src/api.c`, `src/backend_linux.c`, `src/config_manager.c`

**Startup:**
```sh
libreecho-web --backend linux \
    --listen 127.0.0.1:8080 \
    --web-root /usr/local/share/libreecho/web \
    --config /etc/libreecho/web-config.json
```

**Key responsibilities:**
- Serve static files (HTML/CSS/JS) from `--web-root`
- Route `/api/v1/*` requests to backend
- Manage central config (`/etc/libreecho/config.json`)
- Coordinate with companion daemons via adapter protocol
- Log to central logd

**Backend selection:** `--backend mock` (simulated) or `--backend linux` (real hardware).

### Streaming voice-assistant pipeline

The assistant is split at provider-neutral boundaries:

```text
waked post-AEC PCM
  -> sttd local streaming recognition
  -> agentd provider interface
  -> ChatGPT subscription response stream
  -> voice-safe text segmentation
  -> warm ttsd
  -> audiod announcement bus
```

`agentd` consumes indexed 16 kHz PCM continuously after wake detection, so STT
does not wait for a second recording process to start. It sends only the final
transcript to the configured response provider. Response deltas are spoken at
the first natural break after 32 characters, or the next word boundary after
48 characters, which bounds text buffering for the three-second first-audio
target.

The first provider is `openai-codex`, authenticated by ChatGPT device OAuth
against the user's subscription. Provider code supplies auth, refresh, request,
and streaming-event callbacks; no metered API-key fallback exists.

### 2. Adapter Protocol

**Role:** Versioned JSON protocol between web daemon and companion daemons.

**Files:** `src/adapter/adapter.h`, `src/adapter/adapter_client.c`, `src/adapter/adapter_server.c`

**Wire format** (newline-delimited JSON over AF_UNIX SOCK_STREAM):
```json
{"v":1,"id":42,"cmd":"set_volume","args":{"volume":75}}\n
{"v":1,"id":42,"ok":true,"data":{}}\n
```

**Socket paths:**
- `/run/libreecho/network.sock` — networkd
- `/run/libreecho/audio.sock` — audiod
- `/run/libreecho/led.sock` — ledd

**Client usage (backend_linux.c):**
```c
struct le_adapter *a = le_adapter_connect("/run/libreecho/audio.sock", 100);
if (a) {
    char response[4096];
    int rc = le_adapter_call(a, "set_volume", "{\"volume\":75}", response, sizeof(response));
    le_adapter_close(a);
}
```

**Server usage (companion daemon):**
```c
int listen_fd = le_adapter_listen("/run/libreecho/audio.sock");
int client_fd = le_adapter_accept(listen_fd);
char msg[4096];
// read line, parse with le_adapter_parse_request(), respond with le_adapter_respond_ok()
```

### 3. Companion Daemons

Each daemon owns one hardware domain and exposes it via the adapter protocol.

#### networkd — WiFi/Network Management

**Socket:** `/run/libreecho/network.sock`
**Hardware:** wpa_supplicant, udhcpc, netlink

**Commands:**
| Command | Args | Description |
|---------|------|-------------|
| `status` | — | Current network state (SSID, IP, signal) |
| `scan` | — | Trigger WiFi scan, return results |
| `connect` | `{ssid, psk, security}` | Connect to network |
| `disconnect` | — | Disconnect from network |
| `set_hostname` | `{hostname}` | Set system hostname |

**wpa_supplicant interface:** Direct Unix socket to `/var/run/wpa_supplicant/wlan0`. Sends `SCAN`, `ADD_NETWORK`, `SET_NETWORK`, `ENABLE_NETWORK`, `SELECT_NETWORK` commands.

**DHCP:** Forks `/sbin/udhcpc -i wlan0 -n -q` after successful WPA connection.

#### audiod — Audio Control

**Socket:** `/run/libreecho/audio.sock`
**Hardware:** ALSA (`/dev/snd/controlC0`, `/dev/snd/pcmC0D0p`)

**Commands:**
| Command | Args | Description |
|---------|------|-------------|
| `status` | — | Current volume, gain, mute state |
| `set_volume` | `{volume: 0-100}` | Set playback volume |
| `set_gain` | `{gain: 0-100}` | Set microphone gain |
| `set_mute` | `{muted: bool}` | Toggle mic mute |
| `test_tone` | — | Play 440Hz sine wave |
| `cue` | `{first_hz, second_hz, ms}` | Play a bounded two-tone notification cue |

**ALSA interface:** Direct ioctl on `/dev/snd/controlC0`. Enumerates controls, reads/writes values. Falls back to `amixer` if ioctl fails.

#### ledd — LED Ring Control

**Socket:** `/run/libreecho/led.sock`
**Hardware:** sysfs LED class, I2C LED driver (fallback)

**Commands:**
| Command | Args | Description |
|---------|------|-------------|
| `status` | — | Current color, brightness, profiles |
| `set_colour` | `{r, g, b}` | Set RGB color |
| `set_brightness` | `{brightness: 0-100}` | Set brightness |
| `set_boot_profile` | `{r, g, b, brightness}` | Save boot animation |
| `set_profile` | `{name, r, g, b}` | Save named profile |
| `test` | — | Cycle RGB test pattern |
| `animate` | `{profile}` | Start breathing animation |
| `visualizer` | `{action:"frame", levels:"24 hex chars", brightness:0-100, owner}` or `{action:"stop", owner}` | Ephemeral 12-band music overlay |

**Hardware detection:**
1. Try `/sys/class/leds/` (sysfs LED class)
2. Try the IS31FL3236 36-channel frame interface under `/sys/bus/i2c/devices/`
3. Probe accessible `/dev/i2c-*` buses conservatively
4. Fall back to the in-memory stub

The visualizer maps its 12 levels to the 12 physical RGB pixels and expires
after 420 ms without a frame. The LED daemon derives a hysteretic acoustic
mood (`calm`, `balanced`, `energetic`, or `intense`) from energy, spectral
balance, and positive spectral flux. Mood selects the colour family and motion
rate; it is deliberately not presented as genre recognition. Tests and
owner-scoped notification patterns take priority. Non-per-pixel backends
receive a uniform aggregate colour.

### 4. Central Log Daemon (`libreecho-logd`)

**Role:** Aggregate logs from all services into a single rotated file.

**Socket:** `/run/libreecho/log.sock` (SOCK_DGRAM)
**Output:** `/var/log/libreecho/system.log`

**Wire format** (fire-and-forget, newline-delimited JSON):
```json
{"ts":1721476800,"level":"info","service":"networkd","msg":"scan requested"}\n
```

**Rotation:** When file exceeds 512KB, rename to `.1`, `.2`, `.3`. Keeps last 3.

**Usage in any service:**
```c
#include "log.h"
le_log_init("myservice", argc, argv);  // Connects to logd automatically
le_log_info("starting up");
le_log_debug("detail: %d", value);
le_log_error("failed: %s", strerror(errno));
```

**Log levels:** DEBUG < INFO < WARNING < ERROR. Default: INFO.
**Flags:** `--verbose` (DEBUG), `--debug` (DEBUG + source), `--quiet` (WARNING+).

### 5. Configuration Manager

**Role:** Central JSON config with sections per service, SIGHUP reload, history.

**File:** `/etc/libreecho/config.json`

```json
{
  "version": 1,
  "system": { "hostname": "libreecho", "log_level": "info" },
  "audio": { "volume": 50, "microphone_gain": 65 },
  "led": { "brightness": 70, "boot_color": [72, 216, 118] },
  "network": { "wifi_enabled": true, "hostname": "libreecho" },
  "wake_word": { "enabled": true, "sensitivity": 68 },
  "privacy": { "local_only": true, "telemetry": false }
}
```

**API:**
```c
#include "config_manager.h"

le_config_init("/etc/libreecho/config.json");
int volume;
le_config_get_int("audio", "volume", &volume);
le_config_reload();  // Called on SIGHUP
```

**History:** Every write saves previous version to `/etc/libreecho/history/config-<timestamp>.json`. Keeps last 10.

**Reload:** Send `SIGHUP` to any daemon to reload its config section.

### 6. Backup/Restore

**Tool:** `tools/libreecho-backup.sh`

**Create:**
```sh
tools/libreecho-backup.sh create /tmp/backup.tar.gz
```

**Restore:**
```sh
tools/libreecho-backup.sh restore /tmp/backup.tar.gz
```

**Contents:** Config files, recent logs, web state, manifest with version/timestamp/hostname.

## Data Flow Examples

### Setting Volume

```
Browser: PUT /api/v1/audio {"volume":75}
    ↓
libreecho-web: api.c routes to backend_linux.c volume()
    ↓
backend_linux.c: le_adapter_connect("/run/libreecho/audio.sock")
    ↓
audiod: receives {"cmd":"set_volume","args":{"volume":75}}
    ↓
audiod: ALSA ioctl SNDRV_CTL_IOCTL_ELEM_WRITE on /dev/snd/controlC0
    ↓
audiod: responds {"ok":true}
    ↓
backend_linux.c: returns LE_OK
    ↓
libreecho-web: api.c responds 200 {"ok":true,"data":{...}}
    ↓
Browser: updates UI
```

### WiFi Scan

```
Browser: GET /api/v1/network/wifi/scan
    ↓
libreecho-web: backend_linux.c scan()
    ↓
networkd: sends "SCAN\n" to wpa_supplicant
    ↓
wpa_supplicant: triggers scan, returns "OK\n"
    ↓
networkd: polls for "CTRL-EVENT-SCAN-RESULTS"
    ↓
networkd: sends "GET_SCAN_RESULTS\n"
    ↓
networkd: parses results, responds {"ok":true,"data":{"networks":[...]}}
    ↓
libreecho-web: formats JSON response
    ↓
Browser: displays scan results
```

## Error Handling

| Layer | Error | Behavior |
|-------|-------|----------|
| Web daemon → adapter | Read-only status daemon not running | Returns HTTP 200 with `data.available=false` and `data.unavailable=true` |
| Web daemon → adapter | State-changing operation with daemon not running | Returns `LE_NOT_SUPPORTED` → HTTP 501 |
| Adapter → daemon | Daemon returns error | Returns `LE_IO` → HTTP 503 |
| Daemon → hardware | Hardware missing | Returns error in `data.error` → HTTP 500 |
| Config | Invalid JSON | Returns 400 with error message |
| Logging | logd unavailable | Falls back to stderr, continues |

## Security Model

- **Default:** Loopback only (`127.0.0.1:8080`)
- **LAN:** Requires `--auth-token-file` + `--allowed-origin`
- **CSRF:** State-changing requests require the 64-character token returned by `GET /api/v1/config`; it is generated per daemon boot.
- **Confirmation:** Destructive actions require `X-LibreEcho-Confirm: confirm-device-action`
- **Config:** File permissions `0600`, owned by root
- **No raw writes:** Never writes to `/dev/block/*` directly

## Resource Limits

| Resource | Limit | Notes |
|----------|-------|-------|
| HTTP clients | 16 | Per web daemon |
| Request body | 16 KB | JSON API |
| Log file size | 512 KB × 3 | Auto-rotated |
| Config history | 10 versions | Oldest deleted |
| WiFi scan results | 12 | Bounded array |
| Log entries | 128 in memory | Ring buffer |
| Adapter clients | 4 per daemon | Per companion daemon |

## Directory Layout

```
/data/libreecho/config/
├── web-config.json          # Canonical non-secret device configuration
├── agent.json               # Non-secret assistant provider/model/prompt
├── wpa_supplicant.conf      # Wi-Fi credentials (0600, excluded from export)
└── users                    # Local authentication database (0600)

/data/libreecho/secrets/
└── openai-codex.json        # OAuth credentials (0600; never exported)

/etc/libreecho/
├── web-config.json          # Read-only first-boot defaults
├── airplay2.conf            # Packaged integration defaults
└── led-state.json           # Per-boot LED daemon working state

/run/libreecho/
├── network.sock             # networkd adapter socket
├── audio.sock               # audiod adapter socket
├── wakeword.sock            # wake events and indexed post-AEC PCM
├── stt.sock                 # local streaming STT
├── tts.sock                 # warm local TTS
├── agent.sock               # provider/auth/voice-loop control
├── led.sock                 # ledd adapter socket
└── log.sock                 # logd log aggregation socket

/var/log/libreecho/
├── system.log               # Current aggregated log
├── system.log.1             # Previous rotation
└── system.log.2             # Oldest rotation

/usr/local/sbin/
├── libreecho-web            # Main web daemon
├── libreecho-logd           # Log aggregator
├── libreecho-networkd       # WiFi daemon
├── libreecho-audiod         # Audio daemon
└── libreecho-ledd           # LED daemon

/usr/local/share/libreecho/web/
├── index.html
├── css/app.css
├── js/app.js
└── openapi.json
```

## Process Dependencies

```
libreecho-logd      (starts first, others connect to it)
    ↑
libreecho-networkd  (needs logd for logging)
    ↑
libreecho-audiod    (needs logd for logging)
    ↑
libreecho-ledd      (needs logd for logging)
    ↑
libreecho-waked → libreecho-sttd → libreecho-ttsd → libreecho-agentd
    ↑
libreecho-web       (needs all above for full functionality)
```

**Startup order:** logd → networkd → audiod → micd → waked → sttd → ledd →
btd → airplayd → ttsd → agentd → web

**Shutdown order:** reverse startup order.
