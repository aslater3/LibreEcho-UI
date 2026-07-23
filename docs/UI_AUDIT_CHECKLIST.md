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
| `/api/v1/status` | yes | live pass; CPU/temperature are valid; storage now distinguishes mounted usage from unmounted eMMC capacity | verify the storage-aware image after flash |
| `/api/v1/device` | yes | live pass; reports `LibreEcho`/`libreecho` | none |
| `/api/v1/config` | yes | live pass; token is boot-generated, authentication mode is explicit, and LAN development binding is reported truthfully | package a users file for authenticated builds |
| `/api/v1/config/export` | yes | live pass; returns safe partial export when wake-word is absent | keep field list/versioned contract current |
| `/api/v1/audio` | yes | live pass; reports `startup_sound:false` | do not play |
| `/api/v1/led` | yes | live pass; animation state and ring renderer added | verify live polling on next UI image |
| `/api/v1/buttons` | yes | pass | add explicit GET to the API contract |
| `/api/v1/network` | yes | live pass; connected, RSSI, IP and `Zebox 5g` SSID | Overview now includes SSID |
| `/api/v1/network/wifi/scan` | yes | `501` on Linux; failure logged centrally | repair adapter scan path |
| `/api/v1/wake-word` | yes | `501` while adapter is absent | UI now shows an explicit unsupported state |
| `/api/v1/privacy` | yes | pass | persist all fields consistently |
| `/api/v1/integrations` | yes | pass | add endpoint schema validation |
| `/api/v1/system` | yes | live pass; NTP unavailable and clock validity are explicit | add a real time-sync adapter before release |
| `/api/v1/logs`, `/logs/stream` | yes | live pass; bounded central JSON-lines and one-shot SSE stream | keep retention and rotation bounded |
| `/api/v1/diagnostics` | yes | live pass; all daemon sockets/PIDs and wlan0 healthy | none |
| `/api/v1/events` | yes | pass | validate reconnect/last-event behaviour |
| `/openapi.json`, `/swagger.html` | yes | pass | keep generated contract in sync |

## UI pages

- [x] Overview CPU core cards show online state, utilization, and frequency.
- [x] Overview network state includes a real RSSI-derived signal percentage.
- [x] Overview now shows SSID beside connected Wi-Fi state.
- [x] Storage card no longer presents an unmounted recovery rootfs as `0 / 0 MB`; it shows eMMC capacity with usage unavailable.
- [ ] Replace remaining literal `(none)`, `undefined`, `NaN`, and empty values with
  deliberate `Unavailable`/ `Not configured` labels.
- [x] LED page has a rendered 24-segment ring view based on live colour and
  brightness.
- [x] Control centre, API reference, favicon, and docs now use the canonical
  gradient open-ring LibreEcho mark.
- [x] LED view polls live state while an animation is active.
- [x] Network scan handles empty results and adapter errors without a blank panel.
- [x] Unsupported wake-word state is non-actionable in the UI.
- [x] Add a visible login/logout flow for configured local users, with bearer sessions.
- [x] Add a visible service/log health summary.
- [x] Show clock validity and source on the System page instead of only an ambiguous NTP label.
- [x] Make active LED animation state visible as motion in the rendered ring, respecting reduced-motion preferences.

## Platform and service work

- [x] Add local users with salted password verification and expiring sessions.
- [x] Keep bearer-token mode available for scripted administration.
- [x] Start logd, networkd, audiod, ledd, and web automatically after the
  recovery control plane reaches loopback readiness.
- [x] Make startup ordering and each init-script result visible in `/tmp/init.log`.
- [x] Ensure `/api/v1/logs` reads logd output and preserves wall-clock and
  boot-relative timestamps; logd rotation remains bounded.
- [x] Add live PID/socket health to the diagnostics endpoint.
- [x] Add an authenticated user-file packaging/provisioning step for release
  images; no password is stored in this repository.
- [x] Build, verify, flash, and read back the candidate without invoking audio.
- [x] Re-run the read-only endpoint sweep and UI smoke checks after reboot.
- [x] Add host coverage for user login, session validation, logout, invalidation, and rate limiting.

## Newly identified follow-ups

- [x] Replace fixed CSRF development token with a boot-generated value for LAN
  deployments while preserving same-origin protection.
- [x] Add login rate limiting; retain password reset/provisioning workflow as a
  production follow-up.
- [x] Make Linux configuration export include only confirmed-safe persisted
  fields and return a useful result when one adapter is unavailable.
- [ ] Implement the Linux Wi-Fi scan adapter and expose an empty-result state.
- [x] Add endpoint schema/type assertions to the UI smoke test rather than only
  HTTP status assertions; `tests/live_readonly_audit.sh` is the post-flash
  read-only contract sweep.
- [ ] Validate LED animation timing against the hardware daemon and refresh the
  rendered ring while active.
- [x] Replace prompt-based browser login with a visible login/logout surface;
  retain password provisioning/reset as a production follow-up.
- [x] Add boot-relative log time alongside wall-clock time for unsynchronised
  devices.
- [x] Make the API explorer wait for the current CSRF token and reuse the
  Control Centre session instead of prompting.
- [ ] Add authenticated-device smoke coverage to the post-flash sweep using a
  throwaway users file, without placing credentials in the image.
- [x] Distinguish filesystem storage usage from raw block-device capacity in
  Linux telemetry; report unknown used space as `null` rather than zero.
- [ ] Add a read-only storage mount/partition adapter if persistent filesystem
  usage is required by the production image.
- [ ] Replace the in-memory LED stub with the physical ring adapter and retain
  the rendered UI as its read-only state view.
- [ ] Keep the explicit Wi-Fi scan failure visible until the Linux scan adapter
  is implemented; current logs correctly record the request and `501` reason.

## Latest silent validation (2026-07-23)

- Image run `20260723T012025Z-1b002f05f27d-clean-ssh0-ui65d74a69c5fe-e223aea99f30`
  was verified and flashed to slot `a`; boot SHA-256 is
  `f596b25506dc0be8f5003f209ecffdd4e8fb90d59b3be2dcfd30461bb610b6e7`.
- The existing UART reader captured kernel `1b002f05` with `Brought up 4 CPUs`;
  the refined post-boot scan found no CPU boot failure, hard-lockup watchdog,
  panic, Oops, or BUG signature.
- All five daemons autostarted with result `0`; `/api/v1/diagnostics` reports
  their health as `ok`, and central log JSON-lines/SSE checks pass.
- The image reports a 64-character boot-generated CSRF token, truthful
  `lan-development` binding, boot-relative log seconds, and serves UI commit
  `65d74a69`; CPU, temperature, Wi-Fi SSID/RSSI/IP, LED state, system clock
  fields, config export, logs, diagnostics, authentication controls, and SSE
  contracts pass.
- No playback, capture, mixer, LED test, Wi-Fi mutation, restore, or power
  operation was invoked during this validation.
- [x] Add a bounded read-only service status panel to the Logs page.
- [ ] Test UI at narrow/mobile and desktop widths with a browser after the
  device candidate is flashed.

## Live image findings (2026-07-23)

- The current image autostarts all five daemons after loopback; each init result
  is `0`, all expected sockets exist, and diagnostics reports every adapter
  healthy.
- The current image has no users file, so the development UI is intentionally
  unauthenticated LAN mode. Authenticated builds now accept
  `LIBREECHO_WEB_USERS_FILE` and package a mode-`0600` users file.
- Linux Wi-Fi scan returns `501` and records the failure in central logs; this
  is an adapter limitation, not a UI blank-state failure.
- Linux configuration export now returns the available persisted fields with
  `partial: true` and an `unsupported` field list when an optional adapter is
  absent; restore remains intentionally unsupported on Linux.
- `/api/v1/logs/stream` now returns a one-shot `text/event-stream` event rather
  than JSON with an incorrect content type.
- Web-daemon lifecycle and API audit events now feed the same central logd
  stream as the hardware daemons, while the bounded in-memory event history is
  retained for development fallback.
- Log timestamps reflect the device's unsynchronised wall clock (2010-era
  values on this boot); the System endpoint now exposes `clock_valid: false`
  and `clock_source: "unset"` rather than claiming NTP synchronization.

## Silent-validation record

The live sweep that generated this checklist returned HTTP 200 for all core
read-only routes except the expected Linux `501` adapter gaps. No POST, PUT,
test, connect, disconnect, restore, reboot, shutdown, factory-reset, audio,
or capture operation was invoked.
