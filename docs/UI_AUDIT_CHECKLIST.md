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
| `/api/v1/status` | yes | final image live pass; four cores, temperature, and storage semantics are valid; unmounted eMMC reports 3728 MB capacity with unknown usage | add a persistent filesystem adapter when the production layout is defined |
| `/api/v1/device` | yes | live pass; reports `LibreEcho`/`libreecho` | none |
| `/api/v1/config` | yes | live pass; token is boot-generated, authentication mode is explicit, and LAN development binding is reported truthfully | package a users file for authenticated builds |
| `/api/v1/config/export` | yes | live pass; returns safe partial export when wake-word is absent | keep field list/versioned contract current |
| `/api/v1/audio` | yes | live pass; reports `startup_sound:false` | do not play |
| `/api/v1/led` | yes | live pass; animation state and ring renderer added | verify live polling on next UI image |
| `/api/v1/buttons` | yes | final image pass; GET is explicit and unsupported methods return 405 | none |
| `/api/v1/network` | yes | live pass; connected, RSSI, IP and `Zebox 5g` SSID | Overview now includes SSID |
| `/api/v1/network/wifi/scan` | yes | live pass; vendor WEXT fallback returns a typed network list and central logs record the fallback | replace WEXT with nl80211/vendor scan support when the Wi-Fi stack is upgraded |
| `/api/v1/wake-word` | yes | `501` while adapter is absent | UI now shows an explicit unsupported state |
| `/api/v1/bluetooth` and `/bluetooth/*` | yes | host/mock contract implemented; native HCI path awaits the next flashed kernel image | run bounded scan, pair, unpair, disconnect and reboot-persistence tests on hardware |
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
- [x] Add a Bluetooth page with controller state, capabilities, discovery, pairing responses, known devices, unpair, disconnect, connectable, and discoverable controls.
- [ ] Validate Bluetooth discovery and bond persistence on the MT8163 hardware image.

## Platform and service work

- [x] Add local users with salted password verification and expiring sessions.
- [x] Keep bearer-token mode available for scripted administration.
- [x] Start logd, networkd, audiod, ledd, btd, and web automatically after the
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
- [x] Implement the Linux Wi-Fi scan adapter fallback using the MTK driver's
  WEXT scan table and expose a typed empty-result state.
- [x] Add endpoint schema/type assertions to the UI smoke test rather than only
  HTTP status assertions; `tests/live_readonly_audit.sh` is the post-flash
  read-only contract sweep.
- [x] Reject unsupported methods on button, privacy, and integration routes with
  a declared 405 response.
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
- [x] Keep Wi-Fi scan failures explicit while allowing the WEXT fallback to
  return results when the vendor supplicant lacks `SCAN_RESULTS`.

## Latest silent validation (2026-07-23)

- Final flashed run `20260723T025847Z-1b002f05f27d-clean-ssh0-ui950c6c61e452-23359827b43b`
  contains UI commit `950c6c6` and boot SHA-256
  `ebab7dd3375eeaada716769e2e249e55832cf84084f8142ad2e20b041d039afe`.
  The post-flash sweep passed with four online cores, `47 °C`, raw storage
  capacity `3728 MB`, Wi-Fi scan HTTP 200, startup sound disabled, and all
  daemon diagnostics healthy.
- Desktop Firefox rendering was attempted in a clean headless profile but
  remains blocked by the host's `RenderCompositorSWGL` framebuffer failure;
  API/static asset validation passed, but this is not claimed as a pixel-level
  browser pass.

- Image run `20260723T025110Z-1b002f05f27d-clean-ssh0-uia74710a0ffde-4c4d09c6118d`
  was independently verified and flashed to slot `a`; boot SHA-256 is
  `dc9e1bfbba53abd3adb4c4718c2624dc777641d8ca8ef5e34f4c9d20933b7a1c`.
- The live device brought up all four CPUs, reported `47 °C`, and exposed
  `3728 MB` raw eMMC capacity with `storage_available:false` and
  `storage_state:"block-device-unmounted"`; no storage was mounted or written.
- `/api/v1/network/wifi/scan` returned HTTP 200 with a valid network list, and
  central logs recorded `wpa scan unsupported; using WEXT driver results` and
  `WEXT scan results ready`.
- The complete host suite passed before this image build; the post-build API
  check also passed JSON quote/backslash escaping using mock-only button data.
- No playback, capture, mixer, LED test, Wi-Fi connect/disconnect, restore, or
  power operation was invoked during this validation.

- Image run `20260723T015835Z-1b002f05f27d-clean-ssh0-uid99014cf475e-de8183f34911`
  was independently verified and flashed to slot `a`; boot SHA-256 is
  `5e01fe53977b9ea0beccf3201c098bc558d624837d236a4d86629905fd0956cc`.
- The existing UART reader captured kernel `1b002f05` with `Brought up 4 CPUs`;
  the refined post-boot scan found no CPU boot failure, hard-lockup watchdog,
  panic, Oops, or BUG signature.
- All five daemons autostarted with result `0`; `/api/v1/diagnostics` reports
  their health as `ok`, and central log JSON-lines/SSE checks pass.
- The image reports a 64-character boot-generated CSRF token, truthful
  `lan-development` binding, boot-relative log seconds, and serves UI commit
  `d99014cf`. CPU, temperature, Wi-Fi SSID/RSSI/IP, storage semantics,
  LED state, system clock fields, config export, logs, diagnostics,
  authentication controls, SSE, and declared 405 method contracts pass.
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
- Linux Wi-Fi scan uses the WEXT driver table because the vendor supplicant
  rejects both `SCAN` and `SCAN_RESULTS`; the fallback is read-only and does
  not alter association or credentials.
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

The current live sweep returned HTTP 200 for the core read-only routes,
including Wi-Fi scan; wake-word remains an explicit unsupported adapter gap.
No POST, PUT, test, connect, disconnect, restore, reboot, shutdown,
factory-reset, audio, or capture operation was invoked.
