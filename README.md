# LibreEcho web administration interface

LibreEcho Web is a hardware-independent control centre for an Amazon Echo Gen 2-class device. It combines the supplied dark LibreEcho interface with a dependency-free C99 HTTP daemon, a realistic development backend, and a conservative Linux hardware boundary.

## Architecture

The daemon is a single bounded `poll()` event loop: no per-request processes, per-client threads, language runtime, package manager, database, or container is required. It serves static files and `/api/v1/` from the same process.

```text
browser (HTML/CSS/vanilla JS)
          │ JSON /api/v1
          ▼
HTTP limits + security headers + API validation
          │
          ├── config store (0600, temp + fsync + rename + backup)
          ├── bounded log/event rings (128 / 64 entries)
          └── backend.h
                ├── mock: state, transitions, faults, persistence
                └── linux: /proc + statvfs + sysfs; hardware stubs
```

Limits are fixed at 16 clients, 8 KiB headers, 16 KiB API bodies, 12 Wi-Fi scan results, 128 logs, and 64 events. Static paths reject `..` and backslashes. State-changing requests require `X-LibreEcho-CSRF`; device power actions additionally require `X-LibreEcho-Confirm`. The daemon binds to loopback by default and warns on a non-loopback bind. The current authentication abstraction is intentionally development-disabled, so do not expose it to an untrusted LAN yet.

Configuration writes are atomic and only happen on changes. Mock telemetry stays in memory. Stored Wi-Fi passwords are neither returned nor logged. The API consistently returns `{ "ok", "data", "error" }` envelopes.

System → Configuration provides a versioned JSON backup and restore workflow. Exports include configurable audio, microphone, LED, wake-word, hostname, local-access, button, privacy and integration settings. They intentionally exclude Wi-Fi passwords, bearer tokens, logs, diagnostics and live telemetry. Imports are type/range checked as a complete schema before application and are atomically persisted with the same backup behavior as normal configuration writes. The Linux backend returns `not_supported` until all corresponding hardware adapters can safely apply a complete restore.

For authenticated LAN deployment, create a root-readable token file containing at least 16 random characters and declare the exact browser origin:

```sh
libreecho-web --backend linux \
  --listen 0.0.0.0:8080 \
  --auth-token-file /etc/libreecho/api.token \
  --allowed-origin http://libreecho.local:8080 \
  --user libreecho \
  --web-root /usr/local/share/libreecho/web
```

The daemon refuses an unauthenticated non-loopback bind unless the operator explicitly supplies `--allow-insecure-lan`. Bearer tokens are compared in constant time and are never logged. `--user` resolves the account after binding, clears supplementary groups, and then calls `setgid()`/`setuid()`. Ensure the web root remains readable and the configuration directory is writable by that account.

A reverse proxy is not required: the native daemon serves the frontend, API, OpenAPI document and event responses. A small existing LAN proxy may still be used for TLS or centralized authentication, with LibreEcho bound to loopback behind it. nginx is optional rather than a runtime dependency.

## Build and run

Requirements are a C99 compiler, POSIX libc, `make`, and (for API tests) `curl`.

```sh
make
./build/libreecho-web --backend mock --web-root ./web --listen 127.0.0.1:8080
```

For deterministic, isolated UI development:

```sh
./build/libreecho-web \
  --backend mock \
  --mock-config ./config/mock-state.json \
  --config ./build/test-config.json \
  --web-root ./web \
  --listen 127.0.0.1:8080 \
  --seed 42 \
  --dev-controls
```

Open `http://127.0.0.1:8080`. A size-optimised production build compiles out development-control routes:

```sh
make release
```

Cross compilation and staged installation are explicit:

```sh
make CROSS_COMPILE=arm-linux-musleabihf- CC=gcc
make DESTDIR=/tmp/libreecho-root PREFIX=/usr install
```

`init/libreecho-web.init` is the BusyBox/SysV deployment option. `init/libreecho-web.service` is optional for development systems with systemd.

## Backends

Choose the backend at runtime; the browser uses the same API in both cases:

```sh
# Real Linux telemetry plus honest hardware-operation failures
./build/libreecho-web --backend linux --web-root ./web

# Realistic simulated hardware
./build/libreecho-web --backend mock --web-root ./web --seed 42 --dev-controls
```

The mock backend varies CPU, RAM and temperature gradually; scans realistic Wi-Fi networks; models delayed connection, reboot and failure transitions; persists user-facing settings; and accepts deterministic faults. Example development controls:

```sh
tools/mockctl.sh set-temperature 72
tools/mockctl.sh set-wifi disconnected
tools/mockctl.sh fail-next wifi-connect
tools/mockctl.sh trigger wake-word
tools/mockctl.sh set-update-progress 45
tools/mockctl.sh reset
```

Set `LIBREECHO_URL` when using a port other than 8080. These routes exist only in normal development builds and require both `--dev-controls` and the CSRF header.

## API and live state

Implemented v1 areas include status, device, config metadata, audio, LED, buttons, wake word, Wi-Fi scan/connect/disconnect, network identity, privacy, integrations, system/OTA model, logs, diagnostics, events, and guarded power operations. The overview polls once every five seconds. `/api/v1/events` emits bounded SSE-formatted event snapshots, but a persistent multi-client SSE fan-out is deferred; polling avoids pretending that the initial one-shot stream is a full push service.

OTA is a UI/API data model only. The Linux backend returns unsupported for hardware installation. There are no raw writes to `/dev/block/*`.

## Tests

```sh
make test
```

The suite covers JSON/config units, API smoke behavior, malformed JSON, 16 KiB limits, CSRF, destructive confirmation, mock faults and delayed connections, secret redaction, restrictive config mode, restart persistence, Linux unsupported operations, and idle RSS. No Echo hardware is needed. See `tests/browser-checklist.md` for the responsive/accessibility smoke pass.

Latest measurements on the macOS ARM64 development host (19 July 2026):

- Normal daemon binary: 73,184 bytes.
- Size-optimised daemon binary: 73,072 bytes.
- Idle RSS with mock backend: 1,680 KiB.
- Complete uncompressed frontend: about 148 KiB (including the 99 KiB device image).

Host results are indicative; remeasure on the final musl/uClibc target with `size`, `/proc/<pid>/status`, and representative LAN clients.

## Repository tree

```text
.
├── Makefile
├── README.md
├── LICENSE
├── config/{defaults.json,mock-state.json}
├── init/{libreecho-web.init,libreecho-web.service}
├── src/
│   ├── main.c
│   ├── http_server.{c,h}
│   ├── api.{c,h}
│   ├── config_store.{c,h}
│   ├── event_bus.{c,h}
│   ├── backend.{c,h}
│   ├── backend_internal.h
│   ├── backend_mock.c
│   ├── backend_linux.c
│   └── json.{c,h}
├── web/
│   ├── index.html
│   ├── assets/{mark.svg,device.png}
│   ├── css/app.css
│   └── js/app.js
├── tests/
│   ├── run_tests.sh
│   ├── test_unit.c
│   ├── test_api.sh
│   ├── test_config.sh
│   ├── test_mock_behaviour.sh
│   ├── test_limits.sh
│   ├── test_memory.sh
│   └── browser-checklist.md
└── tools/mockctl.sh
```

## Hardware work still required

The following Linux adapter functions deliberately return `not_supported` until the Echo hardware and companion-daemon protocols are researched:

- Audio state, volume, microphone gain/mute and test tone via `/run/libreecho/audio.sock`.
- LED state/profile/brightness/test via `/run/libreecho/led.sock`.
- Wi-Fi scan, credential handoff, connect/disconnect and interface state via `/run/libreecho/network.sock`.
- Wake-word model state, sensitivity and test event via `/run/libreecho/wakeword.sock`.
- Button mappings and physical mute-state input.
- Safe reboot/shutdown/factory-reset platform adapters without assuming systemd.
- Authenticated diagnostics adapters for kernel and service logs.
- Signed A/B update download, verification, inactive-slot installation, boot confirmation and rollback.
- Optional privilege drop after binding, once the production user/group and writable paths are finalized.

Do not add undocumented ioctl numbers. Each integration belongs behind `backend.h` or a small versioned AF_UNIX adapter.

## Known limitations

- Authentication/token provisioning is an abstraction only; loopback is therefore the safe default.
- TLS is expected to terminate at a small trusted LAN proxy if required; no TLS library is bundled.
- SSE is currently a bounded one-shot snapshot and the UI uses five-second overview polling.
- Privacy, integration, button and OTA panels expose the API model, but some settings are not yet persisted independently.
- Static IPv4 configuration, Ethernet, SSH control, restore/upload, and diagnostic bundle streaming need their future adapters.
- Browser screenshots could not be captured in the development session used for these measurements because no controllable browser instance was available; use the supplied checklist when one is available.

## Security notes

This prototype is not authorization to expose a physical-device control plane publicly. Before LAN production deployment, provision per-device authentication, store its secret separately with mode `0600`, add a login/session flow, tighten Origin validation to the configured host, and run the daemon as an unprivileged account. Never pass untrusted values through a shell, accept device paths over HTTP, or write raw partitions from this daemon.
