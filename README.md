# LibreEcho UI

LibreEcho UI is the browser control centre and native service layer for LibreEcho on Amazon Echo Gen 2-class hardware. It contains:

- a dependency-free C99 HTTP daemon;
- a vanilla HTML/CSS/JavaScript frontend;
- a mock backend for deterministic development and API testing;
- a Linux backend for system/network telemetry and companion-daemon adapters;
- BusyBox/SysV and optional systemd service definitions;
- host-verifiable tests and API contracts.

This repository is **source for review and contribution**. It is not a hosted service, a complete device image, or a substitute for the separate LibreEcho product/build repositories.

The root MIT license applies to LibreEcho-authored files unless a file carries
another notice. Vendored SBC code is LGPL-2.1-or-later and retains its upstream
copyright headers. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for
the complete source/runtime boundary and [`SECURITY.md`](SECURITY.md) for
private vulnerability reporting and redaction rules.

## Current state at a glance

The project has several different evidence levels. They must not be confused:

| Area | Current source state | Host/mock evidence | Target/image boundary |
|---|---|---|---|
| Web UI and API | Implemented | API, persistence, auth, limits, OpenAPI, and browser contracts | Served by `libreecho-web` in the image |
| Mock backend | Implemented | Deterministic telemetry, delayed transitions, faults, LED, Wi-Fi, Bluetooth, audio and OTA-unavailable states | Development only; does not prove hardware behavior |
| Linux telemetry | Implemented | Host tests for `/proc`, `/sys`, storage and network parsing | Reports the running target's state |
| Authentication | Implemented | Bootstrap, login, bearer sessions, CSRF, Origin and rate-limit tests | Provision users/tokens outside Git |
| LED control | Implemented and adapter-wired | LED daemon, profiles, brightness, test, startup animation and visualizer contracts | Requires `libreecho-ledd` plus sysfs/I2C hardware support in the image |
| Wi-Fi/network | Implemented and adapter-wired | Scan/liveness/recovery contracts and mock transitions | Requires `networkd`, `wpa_supplicant`, DHCP and target WLAN support |
| Bluetooth | Implemented through `btd` and API contracts | MGMT, pairing, metadata, SDP/A2DP/AVRCP and startup contracts | Controller transport remains target-dependent; a later live attempt failed below the UI at HCI/MGMT bring-up |
| Audio/microphone | Implemented through service boundaries | DSP, shared-capture, stream-format and API contracts | Requires target ALSA/Radar capture and companion services |
| Voice pipeline | Implemented as local/custom/Home Assistant modes | Wyoming, assistant, latency and configuration contracts | Requires the selected STT/TTS/assistant services and credentials |
| OTA/update | Implemented through the live Linux image path | API/channel/authorization contracts; mock deliberately returns unavailable | Requires the signed A/B helper set and image release policy |
| Hardware acceptance | Separate gate | Never inferred from a host build | Must be recorded from the actual target and image |

The source is intentionally honest when a target adapter is absent: the API returns `not_supported` instead of claiming that a mock or unavailable device operation succeeded.

## What is implemented

### Web daemon and API

`libreecho-web` serves the frontend, `/api/v1/`, OpenAPI, and bounded event responses from one process. The event loop uses fixed resource limits:

- 16 HTTP clients;
- 8 KiB request headers;
- 16 KiB JSON request bodies;
- 12 Wi-Fi scan results;
- 128 log entries;
- 64 events.

State-changing requests require `X-LibreEcho-CSRF`; destructive power actions additionally require `X-LibreEcho-Confirm`. Static paths reject traversal and backslashes. Configuration writes use temporary files, `fsync`, atomic rename, backups, and restrictive permissions.

The API currently exposes 64 documented paths across:

- first-boot setup;
- authentication and local user management;
- device/system telemetry and provenance;
- audio, playback, microphone listing and microphone streaming;
- LED, named profiles, buttons and visualizer state;
- wake word;
- Wi-Fi scan/connect/disconnect and network liveness;
- Bluetooth discovery, pairing, bonds and profile state;
- privacy and integrations;
- assistant and speech-pipeline configuration;
- logs, diagnostics and bounded events;
- signed OTA status and update actions;
- guarded reboot, shutdown and factory reset.

See [`docs/API.md`](docs/API.md) and [`web/openapi.json`](web/openapi.json) for the contract.

### Mock backend

The mock backend is the primary development and host-test environment. It provides deterministic simulated hardware state, including:

- changing CPU, memory and temperature telemetry;
- Wi-Fi scans, delayed association, failure and recovery;
- LED colour, brightness, profiles, startup state and visualizer levels;
- Bluetooth discovery, pairing, connection and metadata state;
- audio settings, wake-word state and playback metadata;
- reboot/failure transitions;
- explicit fault injection through the development controls.

Mock behavior is useful for UI and API work but is not hardware evidence.

### Linux backend and companion services

The Linux backend reads system/network telemetry locally and delegates hardware ownership to companion services over bounded AF_UNIX sockets:

```text
libreecho-web
  ├── network.sock  → libreecho-networkd
  ├── audio.sock    → libreecho-audiod
  ├── led.sock      → libreecho-ledd
  ├── wakeword.sock → wake-word service
  ├── btd socket    → libreecho-btd
  ├── agent.sock    → libreecho-agentd
  └── log.sock      → libreecho-logd
```

This keeps the web process out of raw WMT, ALSA, LED-controller, and privileged platform ownership. Missing services return a bounded error, normally HTTP 501 for `not_supported`.

### LEDs

LED support is not merely a UI mock. The repository contains the Linux adapter client, `libreecho-ledd`, its init script, and contracts for:

- sysfs LED operation;
- IS31FL3236 36-channel operation;
- colour and brightness;
- named listening/thinking/error/DND profiles;
- startup animation and readiness hand-off;
- test patterns;
- AirPlay/audio visualizer ownership and timeout behavior.

The target still needs the appropriate LED hardware path and the packaged `libreecho-ledd` service for this to become device behavior.

### OTA and updates

OTA is implemented at the UI/API boundary and is wired for the live Linux image path. The UI exposes:

- signed A/B status;
- current and inactive slots;
- pending activation and rollback state;
- installed/latest versions;
- GitHub Releases reachability and check status;
- stable/dev channel selection;
- automatic-update preference;
- signed update check and apply;
- bounded manual `.tar` upload.

The daemon does not write block devices. It delegates to image-provided helpers:

```text
/usr/local/sbin/libreecho-bootctl
/usr/local/sbin/libreecho-update
/usr/local/sbin/libreecho-update-fetch
```

The production image must provide those helpers, the signed release/channel policy, and the target-side A/B installer. Development and mock images intentionally report OTA as unavailable. A successful host/API test is not proof that a particular OTA image was accepted by hardware.

## What is not complete or is target-dependent

These areas have source/API contracts but are not universally available from a host build or every image:

- real audio volume, microphone gain/mute, test tone and announcement playback;
- Radar microphone capture and browser microphone monitoring;
- Wi-Fi credential handoff and real association/DHCP;
- wake-word hardware/model service;
- physical button and mute-state integration;
- Bluetooth controller transport, discovery and pairing on the target;
- authenticated kernel/service diagnostics;
- static IPv4/Ethernet configuration;
- SSH control;
- complete configuration restore on Linux targets;
- target-specific privilege dropping and writable-path policy;
- signed OTA check/download/apply when the image helper set is absent or unconfigured.

A recorded live Bluetooth attempt reached the UI/API and lower daemon layers but failed at the controller HCI/MGMT transport boundary. That is a Bluetooth/kernel/transport issue, not evidence that the web API itself is absent. Keep this distinction when evaluating new target images.

## Build and run locally

Core requirements:

- C99 compiler and POSIX libc;
- `make`;
- `curl` and `jq` for API tests;
- SpeexDSP development headers/libraries for the complete voice AEC suite;
- additional cross/inference dependencies only for the corresponding ARM32 voice targets.

Build and run the mock backend:

```sh
make
./build/libreecho-web \
  --backend mock \
  --mock-config ./config/mock-state.json \
  --config ./build/test-config.json \
  --web-root ./web \
  --listen 127.0.0.1:8080 \
  --seed 42 \
  --dev-controls
```

Open `http://127.0.0.1:8080`.

For only the main web daemon:

```sh
make build/libreecho-web
```

For Linux telemetry with honest unavailable-adapter responses:

```sh
./build/libreecho-web --backend linux --web-root ./web
```

For the size-optimised production daemon set:

```sh
make release
```

Cross-compilation and staged installation are explicit:

```sh
make CROSS_COMPILE=arm-linux-musleabihf- CC=gcc release
make DESTDIR=/tmp/libreecho-root PREFIX=/usr install
```

The init files are service definitions. Image construction and device deployment are performed by the separate LibreEcho build/product repositories.

## Testing

The normal suite is:

```sh
make test
```

It covers unit behavior, API smoke tests, malformed JSON, body limits, authentication, CSRF and Origin checks, persistence, mock faults, network liveness, Bluetooth protocol contracts, LED ownership/startup/visualizer contracts, microphone fan-out, wake-word UI contracts, OTA channel handling, provenance, public-source safety, and memory behavior.

The full suite requires the host dependencies above. In a minimal development container without SpeexDSP headers, the core build and most contracts can pass while the AEC tests stop at the missing dependency. CI installs the required packages in [`.github/workflows/checks.yml`](.github/workflows/checks.yml).

Additional checks used by CI and release review:

```sh
python3 tests/test_public_source_safety.py
sh tests/test_source_provenance.sh
node --check web/js/app.js
jq -e . web/openapi.json >/dev/null
git diff --check
```

See [`tests/browser-checklist.md`](tests/browser-checklist.md) for the responsive/accessibility pass.

## Security and public-source rules

This repository is public source, not a public device control plane. The daemon binds to loopback by default. For authenticated LAN use, provision a per-device user file or bearer token outside Git, configure the exact allowed Origin, and run the daemon with the least privilege practical. Never use `--allow-insecure-lan` for a production device.

Never commit or package:

- Wi-Fi SSIDs or PSKs from a real environment;
- user passwords, API keys, OAuth credentials, signing keys, or bearer tokens;
- real device serials, MAC addresses, private IPs, or development paths;
- generated userdata, private build manifests, or local deployment evidence.

Public image builds must use a credential-free Wi-Fi profile and receive user-specific network credentials through first-boot setup or another device-local mechanism.

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — component ownership, sockets, data flow and resource limits
- [`docs/API.md`](docs/API.md) — HTTP API reference and examples
- [`docs/HARDWARE.md`](docs/HARDWARE.md) — companion-daemon and hardware adapter guidance
- [`docs/OPERATIONS.md`](docs/OPERATIONS.md) — build, service, configuration, backup and troubleshooting procedures
- [`LICENSE`](LICENSE) — source license

Hardware acceptance, image provenance, OTA publication, flashing, slot confirmation, and rollback evidence belong in the product/build repositories and their release records, not in this UI source README.
