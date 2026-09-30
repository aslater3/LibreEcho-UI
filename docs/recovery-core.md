# Recovery core (networkd, issue #96)

`networkd` owns the secure recovery access point. The portable lifecycle lives in
`src/adapter/network_recovery.[ch]`; `src/adapter/networkd.c` wires it to the
adapter socket, the LED owner protocol and the wpa_supplicant association path.
Nothing here changes account auth/CSRF, factory reset or the one-time setup gate:
the captive web layer still authenticates the owner before any state change.

## Modes

`recovery_status` / the `recovery` object in `status` report one of:
`client`, `armed`, `starting`, `recovery-ap`, `handover`, `unavailable`,
`stopping`, `stopped`. `trigger` is `none`, `physical` (boot marker) or `auto`
(opt-in watchdog). A marker alone never reports `recovery-ap`: the state machine
requires a capability probe, the portal address applied, hostapd + dnsmasq
running and (optionally) a readiness probe.

## Adapter commands

- `status`, `scan`, `connect`, `disconnect`, … unchanged.
- `recovery_status` → the `recovery` object (no secret).
- `recovery_prepare` → ensure the per-device password exists; `{"prepared":true}`.
- `recovery_psk` → owner reveal `{"ssid","psk"}`; **only while client-connected**
  (refused in `starting`/`recovery-ap`, so an unauthenticated captive client can
  never obtain it). The web layer must require an authenticated owner session +
  CSRF before proxying either command; the value is never logged.
- `recovery_stop` → owner stop: children down, portal address removed, marker
  cleared, LED ownership released.

## Config (adapter flags / `/etc/default/libreecho-networkd`)

`--recovery-marker --recovery-psk --recovery-run-dir --recovery-timeout
--recovery-start-timeout --recovery-stop-timeout --recovery-auto
--recovery-disabled --recovery-ap-probe --recovery-ready-probe --recovery-net-up
--recovery-net-down --recovery-address --hostapd --hostapd-conf --recovery-dhcp
--recovery-dns --recovery-conf --led-socket`.

Physical entry is on by default; `--recovery-auto` (opt-in) arms only on a unit
with a saved Wi-Fi profile; `--recovery-disabled` turns the feature off entirely.
`recovery_ap_timeout` is bounded to 30 s–600 s (default 120 s).

## Platform helper contract

The daemon invokes bounded platform helpers and gates on their exit status:

- `libreecho-recovery-ap-probe` → exit 0 iff the driver advertises AP mode.
- `libreecho-recovery-net-up --interface <iface> --address <addr>/24` → assign
  the portal IPv4 and release the radio from client STA management.
- `libreecho-recovery-net-down --interface <iface>` → remove the address and
  return the interface to client management.

A missing/failing net helper or capability probe fails **closed**
(`ap-net-unconfigured`, `ap-net-up-failed`, `ap-driver-unverified`, …) with no
child left running and no false `recovery-ap`.

## Single-interface handover

One radio: hostapd cannot hold the AP while wpa_supplicant associates. On a
credential submission during recovery, `le_recovery_handover_begin()` tears the
children down and runs net-down, then wpa_supplicant attempts association. On
success recovery ends (marker cleared); on failure the AP is rebuilt
(`net-up` + respawn) and the marker is kept so the owner can retry.

## Provisioning secret

Random 32-char WPA2 passphrase from `/dev/urandom`, stored mode 0600 at
`/run/libreecho/recovery-psk`, never serial-derived, never logged, never in
`status`. The owner must save it from the Network page **before** it is needed;
the SSID (`LibreEcho-Setup-<last4-serial>`) is included with the reveal.

## Building the focused tests

```sh
cc -D_POSIX_C_SOURCE=200809L -std=c99 -Wall -Wextra -Wpedantic -Werror -Isrc -Isrc/adapter \
   tests/test_network_recovery.c src/adapter/network_recovery.c -o build/test_network_recovery
./build/test_network_recovery

cc -D_POSIX_C_SOURCE=200809L -DLE_NETWORKD_TESTING -std=c99 -Wall -Wextra -Wpedantic -Werror \
   -Isrc -Isrc/adapter src/adapter/networkd.c src/adapter/network_health.c \
   src/adapter/gateway_probe.c src/adapter/adapter_server.c src/log.c \
   src/adapter/network_recovery.c -o build/test-networkd-recovery
python3 tests/test_network_recovery_lifecycle.py
```

Integrator: add `src/adapter/network_recovery.c` to `NETWORKD_SOURCES` (the daemon
and `test-networkd-health` link against it) and wire both tests into
`tests/run_tests.sh`.
