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

The `recovery` object also carries `address`, the portal IPv4 (`config.ap_address`).
It is not a secret: it is the address the AP leases and DNS-resolves captive
clients to, and the web layer uses it to build the captive-portal redirect.

## Captive portal

Joining the recovery AP must land the owner on the recovery sign-in page, not a
generic index. Two mechanisms do that, both only while `mode == "recovery-ap"`:

- the generated dnsmasq config advertises RFC 8910 DHCP option `114`
  (`dhcp-option=114,http://<address>/?recovery=1`) so a joining client is
  directed to the page without a probe round-trip;
- the web layer answers a captive-probe path (`/generate_204`, `/gen_204`,
  `/hotspot-detect.html`, `/library/test/success.html`, `/connecttest.txt`,
  `/ncsi.txt`, `/canonical.html`, `/success.txt`), a request whose `Host` is not
  the portal address, or a bare `GET /` with `302 Location:
  http://<address>/?recovery=1`, and serves the hinted root as `setup.html`.

The redirect target is the unauthenticated login landing, so it never exposes an
authenticated surface or a secret, and outside `recovery-ap` every request is
served unchanged. `docs/API.md` documents the HTTP contract; the redirect is
proven end-to-end (real `networkd`) by
`tests/run_recovery_backend_integration.sh`.

## Handover re-arm invariant

Any failure on the single-radio handover path — a failed association, a
`SAVE_CONFIG` failure after association, or DHCP that cannot start or lease —
must call `le_recovery_handover_result(...,0)`, which rebuilds the AP and keeps
the marker. Leaving the state machine in `handover` with no AP is unrecoverable:
`le_recovery_tick()` does nothing in that mode. `networkd.c` re-arms on every
such path, and `tests/test_network_recovery_lifecycle.py` pins each one.

## Adapter commands

- `status`, `connect`, `disconnect`, … unchanged.
- `scan` → the kernel scan path: wpa_supplicant while the client plane owns the
  radio, and the AP-forced `NL80211_SCAN_FLAG_AP` scan while the recovery AP
  owns it (wpa_supplicant is stopped by the net-up helper then), so the setup and
  recovery portals' **Scan again** actions keep listing networks during recovery.
- `recovery_status` → the `recovery` object (no secret, **no `psk_path`**).
- `recovery_prepare` → ensure the per-device password exists; `{"prepared":true}`.
- `recovery_psk` → owner reveal `{"ssid","psk"}`; **only while client-connected**
  (refused in `starting`/`recovery-ap`, so an unauthenticated captive client can
  never obtain it). The web layer must require an authenticated owner session +
  CSRF before proxying either command; the value is never logged.
- `recovery_stop` → owner stop: children down, portal address removed, marker
  cleared, LED ownership released.
- `recovery_configure` → owner configuration
  `{"enabled":bool,"auto_enabled":bool,"auto_timeout_ms":30000..600000}`. Strictly
  validated (out-of-range values are rejected, never silently clamped) and
  persisted **before** the running config changes; a persistence failure rolls
  back. On success returns the full status object. See
  `evidence/recovery-runtime-contract.md` for the frozen API mapping.

`recovery_psk`, `recovery_prepare`, `recovery_stop` and `recovery_configure` also
require a root peer (`SO_PEERCRED` uid 0) on the adapter socket, in addition to
the web layer's owner session + CSRF.

## Config (adapter flags / `/etc/default/libreecho-networkd`)

`--recovery-marker --recovery-psk --recovery-config --recovery-run-dir
--recovery-timeout --recovery-start-timeout --recovery-stop-timeout
--recovery-auto --recovery-disabled --recovery-ap-probe --recovery-ready-probe
--recovery-net-up --recovery-net-down --recovery-address --hostapd --hostapd-conf
--recovery-dhcp --recovery-dns --recovery-conf --led-socket`.

Physical entry is on by default; `--recovery-auto` (opt-in) arms only on a unit
with a saved Wi-Fi profile; `--recovery-disabled` turns the feature off entirely.
`--recovery-timeout` (auto window) is bounded to 30 s–600 s (default 120 s); an
owner value through `recovery_configure` outside that range is rejected rather
than clamped. `--recovery-psk` defaults to the persistent
`/data/libreecho/config/recovery-psk`, `--recovery-config` to the persistent
`/data/libreecho/config/recovery.json`; both parents must be protected
(real directory, root-owned in production, not group/world-accessible, no
symlinks). The persisted config is loaded before the boot trigger is evaluated.

## Platform helper contract

The daemon invokes bounded platform helpers and gates on their exit status:

- `libreecho-recovery-ap-probe` → exit 0 iff the driver advertises AP mode
  (`--recovery-ap-probe`).
- `libreecho-recovery-ap-ready` → readiness delegate; exit 0 iff the AP is
  actually serving (interface in AP mode, hostapd control surface up, live
  DHCP/DNS, portal address present). Passed as `--recovery-ready-probe` by
  `init/libreecho-networkd.init`.
- `libreecho-recovery-net-up --interface <iface> --address <addr>/24` → assign
  the portal IPv4 and release the radio from client STA management.
- `libreecho-recovery-net-down --interface <iface>` → remove the address and
  return the interface to client management.

The helper names/paths match the Platform sibling's packaged overlay helpers
(`platform-recovery`). A missing/failing net helper or capability probe fails
**closed** (`ap-net-unconfigured`, `ap-net-up-failed`, `ap-driver-unverified`,
`not-serving`, …) with no child left running and no false `recovery-ap`.

## Single-interface handover

One radio: hostapd cannot hold the AP while wpa_supplicant associates. On a
credential submission during recovery, `le_recovery_handover_begin()` tears the
children down and runs net-down, then wpa_supplicant attempts association. On
success recovery ends (marker cleared); on failure the AP is rebuilt
(`net-up` + respawn) and the marker is kept so the owner can retry.

While the AP owns the interface, `net-up` has **stopped wpa_supplicant**: every
`wpa_ctrl` call fails until `net-down` restarts the control plane. The daemon
therefore does not (re)open the supplicant socket mid-AP (`wpa_open()` returns
early while `net_configured`), and only attempts association after `net-down`.

## Provisioning secret

Random 32-char WPA2 passphrase from `/dev/urandom`, stored mode 0600 at the
persistent path `/data/libreecho/config/recovery-psk` under a protected parent
(atomic `O_EXCL|O_NOFOLLOW` temp + `fsync` + `rename`; never follows a symlink),
never serial-derived, never logged, never in `status`. Because it is persistent,
a password the owner saved during normal client operation stays valid across
reboots. The owner must save it from the Network page **before** it is needed;
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
