# LibreEcho WebUI hardware integration runbook

Audience: an implementation engineer or coding LLM integrating this repository
with a real Amazon Echo Gen 2-class LibreEcho system.

Status: normative for the WebUI boundary. Hardware facts that have not been
verified on a physical device are deliberately marked as research items.

## 1. Objective and completion definition

Replace the `LE_NOT_SUPPORTED` operations in `src/backend_linux.c` with honest,
bounded adapters to real LibreEcho services. Do not change the browser API to fit
the hardware. The mock and Linux backends must continue to implement the same
`backend.h` contract.

The integration is complete only when:

1. the release binary cross-compiles for the target libc/architecture;
2. the daemon starts without systemd, Node.js, Python, containers or a reverse
   proxy;
3. every supported control reads back the state actually accepted by hardware;
4. unsupported hardware still returns `not_supported`, never fabricated success;
5. first boot can enter AP mode, complete setup, join the LAN and close the AP;
6. credentials and tokens are never returned, logged or written to the general
   WebUI configuration;
7. reboot, shutdown, reset and future OTA actions are platform-safe;
8. the test matrix in this document passes on host and target; and
9. measured RSS stays below 15 MiB idle and 25 MiB with several clients.

## 2. Non-negotiable constraints

- Target kernel: Linux 3.18 on MediaTek MT8163.
- Total RAM: 512 MiB; persistent storage is limited.
- Use C99/POSIX and APIs available with the target libc and kernel headers.
- Do not require systemd, glibc extensions, NetworkManager, PulseAudio,
  PipeWire, dbus, Docker, Node.js, Python, Java, PHP, SQLite or a browser on the
  device.
- Do not use io_uring, eBPF, modern cgroups or undocumented ioctl numbers.
- Do not execute shell strings containing request data.
- Do not write `/dev/block/*` or implement OTA partition writes in this phase.
- Do not expose an unauthenticated control plane to a normal LAN.
- Preserve bounded buffers, 16 clients, the single `poll()` loop and the 16 KiB
  API body limit.
- Keep the frontend dependency-free and build-step-free.

If a hardware fact is unknown, stop that adapter at `LE_NOT_SUPPORTED`, add a
specific `TODO(Echo research)` with the missing evidence, and continue with work
that can be verified.

## 3. Repository map and authority

| Path | Responsibility |
|---|---|
| `src/backend.h` | Public hardware abstraction and state structures |
| `src/backend_internal.h` | Backend operation vtable |
| `src/backend.c` | Stable wrapper functions and result codes |
| `src/backend_linux.c` | Real-device integration boundary |
| `src/backend_mock.c` | Behavioural reference, not hardware evidence |
| `src/api.c` | Validation, JSON API, persistence orchestration |
| `src/http_server.c` | Bounded HTTP/static serving and security headers |
| `src/config_store.c` | Atomic general configuration writes |
| `web/openapi.json` | Public HTTP contract |
| `web/index.html`, `web/js/app.js` | Normal control centre |
| `web/setup.html`, `web/js/setup.js` | First-boot AP wizard |
| `init/libreecho-web.init` | Primary BusyBox/SysV launch path |
| `init/libreecho-web.service` | Optional development systemd unit |
| `tests/` | Hardware-free regression suite |

`backend.h`, `api.c` and `openapi.json` are the contract. Do not bypass
`backend.h` from API or frontend code.

## 4. Current implementation baseline

The Linux backend already implements:

- uptime from `/proc/uptime`;
- load information from `/proc/loadavg`;
- RAM totals from `/proc/meminfo`;
- root-filesystem usage through `statvfs()`;
- temperature from `/sys/class/thermal/thermal_zone0/temp`;
- hostname through `gethostname()`;
- immutable board serial from `/proc/idme/serial`; and
- kernel release through `uname()`.

The MT8163 boot image sets the first-boot hostname to
`LibreEcho-<last-four-IDME-serial>` before network services start. Hostname
changes are persisted on userdata and override that default on later boots.

`/data/libreecho/config/web-config.json` is the canonical non-secret device
configuration. Successful configurable PUT operations replace it atomically,
and the Linux control plane reapplies supported audio, LED, music-visualizer,
hostname, wake-word and integration settings at service startup. Wi-Fi
credentials and local users are deliberately excluded from exported JSON;
they remain in mode-0600 persistent stores beside it.

It currently does not implement real audio, LED, Wi-Fi, wake-word or power
controls. `network()` returns an explicit unsupported state. Preserve that
honesty until each adapter is verified.

The browser currently covers:

- first-boot setup;
- overview telemetry and connectivity;
- device identity and power operations;
- audio and microphone controls;
- wake-word settings and test detection;
- LED and button settings;
- Wi-Fi, hostname, SSH and LAN API settings;
- privacy policy;
- integrations;
- OS/OTA placeholder state;
- bounded logs and diagnostics; and
- configuration backup and restore.

## 5. Required integration architecture

Use small device daemons when hardware ownership, blocking I/O or privileged
operations should not live in the web process:

```text
LAN browser
    |
    | HTTP /api/v1
    v
libreecho-web (unprivileged after bind)
    |
    | bounded versioned AF_UNIX requests
    +-- /run/libreecho/audio.sock     -> audio service/ALSA adapter
    +-- /run/libreecho/led.sock       -> LED/button service
    +-- /run/libreecho/wakeword.sock  -> local wake-word service
    +-- /run/libreecho/network.sock   -> Wi-Fi/AP/network supervisor
    +-- optional platform.sock        -> guarded power/platform service
```

Direct, read-only `/proc` and known sysfs reads may remain in
`backend_linux.c`. Device ownership, credential storage, long-running scans,
audio pipelines and privileged mutations belong in dedicated services.

Do not spawn a process per HTTP request. Do not invoke `sh -c`. If a vendor
helper is unavoidable, the owning device daemon must execute a fixed absolute
path with a fixed argv layout after validating every argument.

## 6. Proposed AF_UNIX protocol

This is a LibreEcho internal protocol, not a claim about Amazon firmware.
Implement it consistently in the WebUI client and new device daemons.

### 6.1 Transport

- `AF_UNIX`, `SOCK_STREAM`.
- One request and one response per connection, then close.
- UTF-8 JSON object followed by `\n`.
- Maximum request or response: 4096 bytes unless an adapter documents a smaller
  limit.
- Set non-blocking connect/read/write and use `poll()` with an absolute timeout.
- Normal state operations must finish within 250 ms.
- Mutating operations must acknowledge within 500 ms and perform lengthy work
  asynchronously.
- Wi-Fi scan returns the supervisor's bounded cache and requests a background
  refresh; it must not block the WebUI event loop for a radio scan.
- Never retry a state-changing request automatically unless it has an operation
  ID and the server guarantees idempotence.

### 6.2 Envelope

Request:

```json
{"version":1,"id":42,"operation":"get_state","arguments":{}}
```

Successful response:

```json
{"version":1,"id":42,"ok":true,"result":{}}
```

Error response:

```json
{
  "version":1,
  "id":42,
  "ok":false,
  "error":{"code":"not_supported","message":"Amplifier control is unavailable"}
}
```

Allowed error mapping:

| Socket error code | `le_result` |
|---|---|
| `invalid_request` | `LE_INVALID` |
| `not_supported` | `LE_NOT_SUPPORTED` |
| `busy` | `LE_BUSY` |
| `forbidden` | `LE_AUTH` |
| connection, timeout, malformed response, other | `LE_IO` |

Never copy a daemon error message directly into logs if it could include a
credential or token. The HTTP API already maps result codes to safe messages.

### 6.3 Socket security

- Create `/run/libreecho` at boot if `/run` exists; otherwise use a configured
  volatile directory such as `/var/run/libreecho`.
- Socket owner should be the owning service; group should be `libreecho`.
- Recommended socket mode: `0660`; runtime directory mode: `0750`.
- Verify the peer where supported, but do not require Linux features absent from
  the target libc without a fallback.
- Never place sockets or secrets in the web root.
- Treat symlinked socket paths as a deployment error.

Implement a small `src/unix_rpc.c/.h` shared client with fixed buffers and unit
tests. Keep hardware-specific response parsing in `backend_linux.c` or narrowly
named adapter files.

## 7. Backend operation matrix

Implement the `struct le_backend_ops` entries in this order.

### 7.1 System status and device identity

Functions:

- `le_get_system_status()`
- `le_get_device_info()`

Required work:

1. Retain existing `/proc` and `statvfs()` readers.
2. Discover the correct thermal zone by inspecting zone `type` files; make the
   chosen path configurable. Do not assume zone 0 on every build.
3. Clamp corrupt percentages and temperature readings to defensible ranges or
   report unavailable state.
4. Read OS version from a small LibreEcho release file if present.
5. Read model, serial and hardware revision only from verified device-tree,
   procfs, sysfs or LibreEcho manufacturing data. The current MT8163 serial
   source is `/proc/idme/serial`; if it is unavailable or malformed, return
   `"unavailable"` rather than inventing an identifier.
6. CPU percentage is currently a load-average approximation. Label it as such or
   implement bounded `/proc/stat` deltas in the single backend tick.

Acceptance: `/api/v1/status` and `/api/v1/device` remain responsive if any
individual proc/sysfs file is missing.

### 7.2 Audio and microphones

Socket: `/run/libreecho/audio.sock`

Operations and fields:

| Backend call | Socket operation | Required result |
|---|---|---|
| `le_get_audio_state` | `get_state` | volume, microphone gain/mute, notification volume, startup sound, amplifier and output availability |
| `le_set_volume` | `set_volume` | accepted value 0-100 and read-back |
| `le_set_microphone_gain` | `set_microphone_gain` | accepted value 0-100 and read-back |
| `le_set_microphone_muted` | `set_microphone_muted` | actual mute state |
| `le_play_test_tone` | `play_test_tone` | quick acknowledgement |

The audio daemon may use ALSA or verified MediaTek controls, but the WebUI must
not depend on ALSA libraries. Map WebUI percentages to verified hardware ranges
inside the audio daemon. Hardware mute and software mute must not be conflated.
If gain or amplifier control is unknown, return `not_supported` for that control.

Do not generate a test tone in the HTTP process. Do not block until playback
finishes.

### 7.3 LED ring and buttons

Socket: `/run/libreecho/led.sock`

Operations:

- `get_state`
- `set_colour` with integer `r`, `g`, `b` in 0-255
- `set_brightness` in 0-100
- `set_visualizer_enabled` with an explicit boolean
- `set_boot_profile`
- `run_test`
- `visualizer` with 12 hexadecimal spectral levels, brightness and owner; the
  daemon derives a stable acoustic mood for palette and motion selection
- future `get_buttons` / `set_buttons`

The Biscuit IS31FL3236 path writes 12 RGB pixels through its 36-channel sysfs
frame. Audio frame/mood state is an ephemeral, auto-expiring overlay; only the
user's visualizer-enabled preference is persisted. Put hardware ordering and
brightness conversions in the LED daemon. Do not encode guessed ioctl values
in `backend_linux.c`.

Button actions are currently WebUI policy stored by `api.c`; physical button and
hardware-mute state need an input adapter. Hardware mute indication must always
reflect the physical privacy circuit where one exists.

### 7.4 Network, Wi-Fi and AP supervisor

Socket: `/run/libreecho/network.sock`

Operations:

- `get_state`
- `scan_cached`
- `connect`
- `disconnect`
- `set_hostname`
- `get_ap_state`
- `start_setup_ap`
- `finish_setup_handoff`
- future `set_ssh` and `set_api_lan`

State must include connection state, SSID, signal, IPv4/IPv6 address where
available, gateway, DNS, hostname, Internet reachability and DHCP mode. Limit
scan results to `LE_MAX_WIFI` and sanitize SSIDs as untrusted byte strings.

Credential requirements:

- The WebUI sends a password only in the `connect` request.
- The HTTP request body is scrubbed after handling.
- The network daemon owns credential storage, preferably under
  `/etc/libreecho/secrets/` with directory mode `0700` and file mode `0600`.
- Do not put passwords in `/etc/libreecho/web-config.json`, argv, environment,
  logs, process titles or responses.
- Redact SSIDs only if local policy treats them as secret; always redact keys.

NetworkManager is not allowed. Integrate with the verified target Wi-Fi stack,
`wpa_supplicant` control socket, or a small LibreEcho radio daemon. Never build a
shell command by concatenating SSID or password.

### 7.5 First-boot AP lifecycle

First boot is represented by absent WebUI configuration or
`"setup_completed": false`. Existing configurations without the key are treated
as completed for upgrade compatibility.

Required supervisor sequence:

1. Boot and create the volatile runtime directory.
2. Start required hardware daemons.
3. Read the WebUI configuration without modifying it.
4. If setup is incomplete, start an isolated setup AP using a verified radio
   path.
5. Provide DHCP and captive-portal DNS/HTTP routing using target-available small
   services. Do not add a large dependency solely for captive portal support.
6. Start `libreecho-web` on the setup address, normally port 80, with
   `--allow-insecure-lan` only because this is an isolated setup network. Bind
   first and use `--user libreecho` to drop privileges where available.
7. The browser receives `/setup.html` from `/` and posts `/api/v1/setup`.
8. `le_connect_wifi()` hands credentials to the network daemon. The daemon
   acknowledges `connecting` quickly and owns the radio transition.
9. Wait for station association and a usable address. Only then close the AP if
   the radio cannot maintain AP+STA concurrently.
10. Restart/rebind the WebUI using authenticated LAN policy.
11. If association fails, restore the setup AP and preserve an actionable failure
    state without exposing the password.

The setup API also applies hostname, initial audio volume, wake word and privacy
defaults. Therefore Linux implementations for network, hostname, audio and
wake-word setters are prerequisites for completing the wizard. Do not return
fake success to bypass this dependency.

Captive-portal probe paths vary by client OS. The network supervisor or a tiny
front listener may redirect unknown HTTP paths to `/`; do not weaken static path
validation in `http_server.c`.

### 7.6 Wake word

Socket: `/run/libreecho/wakeword.sock`

Operations:

- `get_state`
- `set_model`
- `set_sensitivity`
- `trigger_test`

State includes enabled, model name/status, sensitivity, cooldown, detection
count, estimated CPU/RAM cost and local-processing status. Model loading and
audio access belong in the wake-word service. `trigger_test` should exercise the
event path without recording or uploading microphone audio.

If a selected model is absent, return `busy` while installing/loading only when
that transition is real; otherwise return `not_supported` or `invalid_request`.

### 7.7 Privacy and integrations

Privacy and integration toggles currently live in `api_context` and the atomic
WebUI JSON configuration. For real integration:

- define which service consumes each setting;
- send an explicit reload/apply request to that service;
- report failure if enforcement cannot be confirmed;
- never imply that `local_only` is enforced until all speech/service paths obey
  it; and
- keep cloud integrations disabled by default.

Home Assistant, MQTT, REST and Bluetooth need separate, narrowly scoped service
adapters. Enabling a bit in the WebUI file alone is not proof that an integration
is running. Extend API state with honest connection status before presenting it
as connected.

SSH and LAN API controls also need privileged platform enforcement. Until those
adapters exist, keep the control disabled or return `not_supported`; do not just
persist a boolean and claim success.

### 7.8 Power and factory reset

Functions:

- `le_reboot()`
- `le_shutdown()`
- `le_factory_reset()`

Use a fixed privileged platform service or verified init mechanism. Do not assume
systemd. The HTTP API already requires CSRF, a confirmation token and rate
limiting.

Factory reset must be recoverable and narrowly scoped:

1. remove WebUI configuration and its backup;
2. remove Wi-Fi and integration secrets from their dedicated stores;
3. preserve bootloader, recovery and manufacturing/calibration data;
4. sync filesystems;
5. request a controlled reboot; and
6. return to first-boot AP mode.

Never recursively delete a broad directory. Enumerate exact files in the
platform service.

### 7.9 Logs and diagnostics

The HTTP daemon has a bounded 128-entry in-memory log ring. Add adapters for
kernel and service diagnostics only after defining redaction.

- Do not expose arbitrary file paths.
- Do not run `dmesg` per request; use a bounded service or proc/kmsg adapter with
  appropriate privilege separation.
- Redact Wi-Fi keys, API tokens, MQTT credentials, URLs containing secrets and
  captured audio paths.
- Stream future diagnostic archives; do not buffer an archive in the 32 KiB API
  response object.
- Apply a maximum archive size and a fixed allowlist of inputs.

### 7.10 OTA

The UI models A/B state only. The Linux backend must remain unsupported until
partition layout, boot control, signature verification and rollback are proven.

Do not:

- accept a raw device path from HTTP;
- write a block device from the WebUI daemon;
- invent partition names or bootloader variables; or
- mark an update installed before verification and boot-control confirmation.

## 8. Configuration ownership

General WebUI configuration:

```text
/etc/libreecho/web-config.json       mode 0600
/etc/libreecho/web-config.json.bak   mode 0600
```

It is written using temporary file, `fsync()` and rename. It may contain audio,
LED, wake-word, hostname, local-access, button, privacy, integration and setup
state. It must not contain Wi-Fi passwords, bearer tokens, logs or telemetry.

Recommended separation:

```text
/etc/libreecho/secrets/wifi.json     network service only, 0600
/etc/libreecho/secrets/api.token     WebUI token, 0600
/etc/libreecho/secrets/mqtt.json     integration service only, 0600
```

Do not make the unprivileged WebUI user owner of every device secret. Prefer
one-way handoff over the protected socket.

Configuration import is currently supported only by the mock backend because a
real restore must be transactional across services. Implement a Linux restore
coordinator before enabling it: validate the complete document, preflight each
service, apply, read back, commit, and roll back on failure.

## 9. Implementation sequence for another LLM

Follow these phases in order. Keep each phase buildable and testable.

### Phase A: inventory, no mutations

On the target, collect and commit a redacted report containing:

```sh
uname -a
cat /proc/cpuinfo
cat /proc/meminfo
cat /proc/loadavg
cat /proc/uptime
find /sys/class/thermal -maxdepth 2 -type f -print
find /sys/class/net -maxdepth 2 -type f -print
find /dev -maxdepth 2 -type c -o -type b
```

Also inventory init, libc, ALSA, Wi-Fi helpers, Unix sockets, device-tree nodes,
mount layout and writable locations. Redact MAC addresses, serials and keys in
published reports. Do not probe unknown character devices with guessed ioctls.

### Phase B: portable RPC client

1. Add fixed-buffer Unix RPC client files.
2. Add timeout, malformed-response, oversized-response and unavailable-socket
   tests using fake local socket servers.
3. Verify no request creates a thread or child process.
4. Keep every backend call bounded.

### Phase C: read-only accuracy

1. Improve thermal and device identity discovery.
2. Implement network read state.
3. Implement audio/LED/wake-word read state.
4. Compare API output with direct target evidence.
5. Keep mutations unsupported until read-back is trustworthy.

### Phase D: reversible controls

Implement and test volume, mic gain/mute, LED colour/brightness/test, wake-word
selection/sensitivity, Wi-Fi scan/connect/disconnect and hostname. After every
mutation, read back actual state. Test invalid ranges and service absence.

### Phase E: first boot and LAN security

Implement the AP supervisor flow, secret store, failure recovery, authenticated
LAN bind and privilege drop. Test with power loss during each transition.

### Phase F: privileged/destructive operations

Implement power and factory reset only after a recovery path is proven. OTA
remains out of scope until separately reviewed.

### Phase G: persistence and diagnostics

Coordinate service configuration, real integration state, bounded logs and
streaming diagnostic export.

## 10. Build and deployment

Host validation:

```sh
make clean
make
make test
make release
```

Cross-build example; adjust the toolchain prefix to the verified target ABI:

```sh
make clean
make CROSS_COMPILE=arm-linux-musleabihf- CC=gcc release
```

Do not assume that `musleabihf` is correct until the device ABI, float ABI and
dynamic loader are verified with the target toolchain.

Staged install:

```sh
make DESTDIR=/tmp/libreecho-root PREFIX=/usr install
```

Expected installed paths:

```text
/usr/sbin/libreecho-web
/usr/share/libreecho/web/
/etc/libreecho/web-config.json
```

For BusyBox/SysV, adapt `init/libreecho-web.init`. Avoid command substitution or
shell interpolation of request-derived data. Ensure PID/log/runtime directories
exist even if `/run` does not.

Normal LAN launch should resemble:

```sh
/usr/sbin/libreecho-web \
  --backend linux \
  --config /etc/libreecho/web-config.json \
  --web-root /usr/share/libreecho/web \
  --listen 0.0.0.0:8080 \
  --auth-token-file /etc/libreecho/secrets/api.token \
  --allowed-origin http://libreecho.local:8080 \
  --user libreecho
```

The daemon refuses unauthenticated non-loopback binds unless
`--allow-insecure-lan` is explicit. Use that exception only on an isolated
first-boot AP with network isolation.

No nginx is required. A small existing reverse proxy may be used for TLS or
central authentication, with the WebUI bound to loopback.

## 11. Test matrix

### 11.1 Mandatory host tests

Run `make test`. It covers JSON/config units, API behaviour, setup, persistence,
limits, CSRF, authentication/origin checks, mock transitions, Linux unsupported
behaviour and RSS.

Add tests for each Linux RPC adapter using fake sockets. Required cases:

- success and exact field mapping;
- socket absent;
- timeout;
- truncated/malformed JSON;
- oversized response;
- wrong protocol version or request ID;
- `not_supported`, `busy` and invalid values;
- password absent from response/log/config;
- daemon reconnect after service restart; and
- bounded repeated calls without file-descriptor or RSS growth.

### 11.2 Target read tests

- API telemetry matches `/proc` within documented rounding.
- Missing thermal sensor does not crash or fabricate a temperature.
- Network state matches the actual interface.
- Audio/LED/wake-word reads match service state.
- All endpoints respond within their timeout budget.

### 11.3 Target mutation tests

- Values 0, midpoint and maximum apply and read back.
- Out-of-range values return `invalid_request`.
- Service stopped returns structured error without killing the WebUI.
- Wi-Fi scan is bounded and does not freeze other clients.
- Failed Wi-Fi connection restores AP mode during setup.
- Physical microphone mute cannot be overridden by software.
- Factory reset removes only enumerated configuration/secrets and returns to AP
  setup.

### 11.4 Security tests

- Normal LAN bind without authentication is refused.
- Wrong bearer token returns 401.
- Wrong Origin and missing CSRF return 403.
- Destructive calls require confirmation and rate limiting.
- Static traversal and arbitrary file access fail.
- SSIDs containing quotes, backslashes, control bytes and shell metacharacters do
  not escape JSON, argv or logs.
- Passwords/tokens do not appear in config, backup, logs, diagnostics, argv,
  environment or core dumps.
- Socket permissions prevent an unrelated local user from controlling hardware.

### 11.5 Performance tests

Record on target:

- release binary size;
- idle RSS after five minutes;
- RSS with at least five browser clients;
- CPU while overview polling;
- latency for every endpoint; and
- flash writes during rapid UI adjustments.

## 12. Acceptance report template

The integrating LLM must finish with a report containing:

```text
Target image/build:
Kernel:
Libc/toolchain:
Commit tested:
Binary size:
Idle RSS / five-client RSS:

Adapter             Read  Write  Tests  Evidence
system/device       ...   n/a    ...    ...
audio/microphone    ...   ...    ...    ...
LED/buttons         ...   ...    ...    ...
network/AP          ...   ...    ...    ...
wake word           ...   ...    ...    ...
privacy/integration ...   ...    ...    ...
power/reset         ...   ...    ...    ...
logs/diagnostics    ...   ...    ...    ...
OTA model           ...   n/a    ...    unsupported by design

Known unsupported operations:
Hardware evidence still required:
Security tests:
Recovery procedure exercised:
```

Do not mark an adapter complete without a target-side observation or service
test proving it.

## 13. Known design gaps to resolve during integration

- The Linux network backend currently reports only hostname and unsupported
  state.
- Linux configuration restore needs a transactional multi-service coordinator.
- SSH/LAN API toggles are persisted policy but do not yet enforce services.
- Button mappings and physical mute input need a hardware service.
- Privacy/integration UI needs actual service enforcement and connection state.
- Persistent multi-client SSE fan-out is not implemented; the UI uses bounded
  polling.
- Diagnostic archive streaming is not implemented.
- AP DHCP/DNS/captive-portal components are not selected because the target
  userspace has not been inventoried.
- Power/reset needs a platform service independent of systemd.
- OTA partition and boot-control research is intentionally not implemented.

Resolve these with evidence and narrow adapters. Do not solve them by weakening
API truthfulness, security defaults or resource bounds.
