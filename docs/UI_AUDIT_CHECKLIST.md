# LibreEcho UI/API audit checklist

This is the working checklist for the ARM32 development image. Validation is
split between host/mock checks and read-only checks against the live device.
Audio playback, microphone capture, LED tests, Wi-Fi connect/disconnect, power
actions, and configuration restore are excluded from silent validation unless
explicitly requested.

## Endpoint inventory

| Endpoint | Read-only check | Current result | Follow-up |
|---|---:|---|---|
| `/api/v1`, `/api/v1/` | yes | pass | none |
| `/api/v1/status` | yes | pass | CPU field naming covered |
| `/api/v1/device` | yes | pass; source now falls back to `LibreEcho`/`libreecho` | verify after flash |
| `/api/v1/config` | yes | pass | users/bearer mode now reported |
| `/api/v1/config/export` | yes | `501` on Linux | implement a complete Linux-safe export |
| `/api/v1/audio` | yes | pass; source now reports the no-startup-audio marker | verify after flash; do not play |
| `/api/v1/led` | yes | pass | animation state and ring renderer added |
| `/api/v1/buttons` | yes | pass | add explicit GET to the API contract |
| `/api/v1/network` | yes | connected/RSSI pass, source now adds WEXT ESSID fallback | verify after flash |
| `/api/v1/network/wifi/scan` | yes | `501` on Linux | repair adapter scan path |
| `/api/v1/wake-word` | yes | `501` while adapter is absent | make unsupported state clear in UI |
| `/api/v1/privacy` | yes | pass | persist all fields consistently |
| `/api/v1/integrations` | yes | pass | add endpoint schema validation |
| `/api/v1/system` | yes | pass, currently static metadata | label unsupported OTA fields honestly |
| `/api/v1/logs`, `/logs/stream` | yes | pass; source reads central JSON-lines logd output with memory fallback | verify daemon startup produces entries |
| `/api/v1/diagnostics` | yes | source now reports PID/socket and wlan0 health | verify after flash |
| `/api/v1/events` | yes | pass | validate reconnect/last-event behaviour |
| `/openapi.json`, `/swagger.html` | yes | pass | keep generated contract in sync |

## UI pages

- [x] Overview CPU core cards show online state, utilization, and frequency.
- [x] Overview network state includes a real RSSI-derived signal percentage.
- [ ] Overview should show SSID beside connected Wi-Fi state after the new
  adapter fallback is flashed.
- [ ] Replace remaining literal `(none)`, `undefined`, `NaN`, and empty values with
  deliberate `Unavailable`/ `Not configured` labels.
- [x] LED page has a rendered 24-segment ring view based on live colour and
  brightness.
- [ ] LED view should refresh while an animation is active.
- [ ] Network scan should handle an empty result without a blank panel.
- [ ] Unsupported wake-word/audio operations need a non-actionable UI state.
- [x] Add a login flow for configured local users, with bearer sessions.
- [ ] Add a visible service/log health summary.

## Platform and service work

- [x] Add local users with salted password verification and expiring sessions.
- [x] Keep bearer-token mode available for scripted administration.
- [x] Start logd, networkd, audiod, ledd, and web automatically after the
  recovery control plane reaches loopback readiness.
- [x] Make startup ordering and each init-script result visible in `/tmp/init.log`.
- [x] Ensure `/api/v1/logs` reads logd output and preserves timestamps; logd
  rotation remains bounded.
- [x] Add live PID/socket health to the diagnostics endpoint.
- [ ] Add an authenticated user-file packaging/provisioning step for release
  images; no password is stored in this repository.
- [ ] Build, verify, flash, and read back the candidate without invoking audio.
- [ ] Re-run the read-only endpoint sweep and UI smoke checks after reboot.

## Newly identified follow-ups

- [ ] Replace fixed CSRF development token with a boot-generated value for LAN
  deployments while preserving same-origin protection.
- [ ] Add login rate limiting and a password reset/provisioning workflow before
  production use.
- [ ] Make Linux configuration export include only confirmed-safe persisted
  fields and return a useful result when one adapter is unavailable.
- [ ] Implement the Linux Wi-Fi scan adapter and expose an empty-result state.
- [ ] Add endpoint schema/type assertions to the UI smoke test rather than only
  HTTP status assertions.
- [ ] Validate LED animation timing against the hardware daemon and refresh the
  rendered ring while active.
- [ ] Add a bounded read-only service status panel to the Logs page.
- [ ] Test UI at narrow/mobile and desktop widths with a browser after the
  device candidate is flashed.

## Silent-validation record

The live sweep that generated this checklist returned HTTP 200 for all core
read-only routes except the expected Linux `501` adapter gaps. No POST, PUT,
test, connect, disconnect, restore, reboot, shutdown, factory-reset, audio,
or capture operation was invoked.
