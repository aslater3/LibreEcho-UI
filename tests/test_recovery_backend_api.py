#!/usr/bin/env python3
"""Drive the recovery API end to end through the real Linux backend.

The gap this closes (integration review F3): the API test only ever drove
`build/libreecho-web --backend mock`, and the lifecycle test only spoke to
networkd directly. Nothing exercised

    HTTP -> api.c -> backend.c -> backend_linux.c --AF_UNIX--> networkd

for the owner recovery routes, so the real backend's argument shaping
(`timeout_seconds * 1000` -> `auto_timeout_ms`) and its error-code mapping
(the 409/501/503 map for `recovery_prepare`) were unverified against a real
daemon.

This test starts a real `libreecho-web` whose network adapter socket is a
private fixture path (compiled in, never `/run/libreecho`), and a real
networkd compiled from src/adapter/networkd.c in LE_NETWORKD_TESTING mode, then
drives the frozen HTTP routes over loopback. Hardware is isolated exactly as in
tests/test_network_recovery_lifecycle.py: the hostapd/dnsmasq/net-up oracles
are shell scripts in a temp directory, no radio, interface, DHCP server or host
network is touched.

The networkd fixture build exempts the production *root-peer* gate (see the
LE_NETWORKD_TESTING note in networkd.c). That gate is exercised for real, with
a production build, by tests/test_recovery_peer_gate.py -- it is deliberately
not silently covered here.

Environment (set by tests/run_recovery_backend_integration.sh):
    LE_F3_WEB        private libreecho-web (network socket compiled in)
    LE_F3_NETWORKD   networkd fixture (LE_NETWORKD_TESTING)
    LE_F3_WORKSPACE  directory whose network.sock the web binary targets

Exits non-zero on any failure.
"""

import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import test_network_recovery_lifecycle as lc  # noqa: E402


def env_path(name, default=None):
    value = os.environ.get(name)
    if value:
        return Path(value)
    if default is not None:
        return default
    raise SystemExit(f"missing required environment variable {name}")


WEB = env_path("LE_F3_WEB")
NETWORKD = env_path("LE_F3_NETWORKD")
WORKSPACE = env_path("LE_F3_WORKSPACE")
lc.BINARY = NETWORKD

# networkd's real mode vocabulary (docs/recovery-core.md). The mock-only
# sentinel "off" is deliberately not here: the point of this suite is that a
# real backend reports networkd's strings, not the mock's.
REAL_MODES = {"client", "armed", "starting", "recovery-ap", "handover",
              "unavailable", "stopping", "stopped"}


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Harness:
    """One networkd fixture + one web daemon sharing a single socket path."""

    def __init__(self, *, marker=False, config_blocker=False, start_networkd=True,
                 net_down_fail_budget=0):
        WORKSPACE.mkdir(parents=True, exist_ok=True)
        # Fresh name space: stale sockets from an earlier case would bind-collide.
        for path in list(WORKSPACE.glob("*.sock")):
            path.unlink()
        for name in ("recovery.json", "recovery.json.bak", "recovery-psk",
                     "recovery-mode", "net-down.log", "net-down.calls",
                     "net-up.log"):
            path = WORKSPACE / name
            if path.is_dir():
                shutil.rmtree(path)
            elif path.exists() or path.is_symlink():
                path.unlink()
        if config_blocker:
            # A directory sitting where the persisted config file belongs makes
            # networkd's atomic write fail ("config-rename-failed") -- a real
            # persist failure, not a simulated return code.
            (WORKSPACE / "recovery.json").mkdir()
        self.fixture = lc.RecoveryFixture(
            WORKSPACE, marker=marker,
            net_down_fail_budget=net_down_fail_budget)
        self.port = free_port()
        self.web_log = open(WORKSPACE / "web-api.log", "ab")
        self.web = None
        try:
            config = WORKSPACE / "web-config.json"
            config.write_text("{}")
            if start_networkd:
                self.web = subprocess.Popen(
                    [str(WEB), "--backend", "linux", "--config", str(config),
                     "--web-root", str(ROOT / "web"),
                     "--listen", f"127.0.0.1:{self.port}"],
                    cwd=ROOT, stdout=self.web_log, stderr=self.web_log,
                    stdin=subprocess.DEVNULL)
                self.csrf = self._await_csrf()
        except BaseException:
            # Never leak the fixture/web if startup fails partway.
            self.close()
            raise

    def _await_csrf(self, timeout=15.0):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            try:
                status, text = self.http("GET", "/api/v1/config",
                                         authenticated=False)
                if status == 200:
                    return json.loads(text)["data"]["csrf_token"]
                last = (status, text[:120])
            except (OSError, ValueError) as error:  # noqa: PERF203 - readiness poll
                last = repr(error)
            time.sleep(0.05)
        raise AssertionError(f"web daemon did not become ready: {last}")

    def http(self, method, path, body=None, authenticated=True):
        headers = {}
        data = None
        if authenticated:
            headers["Content-Type"] = "application/json"
            headers["X-LibreEcho-CSRF"] = self.csrf
        if body is not None:
            data = body.encode()
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.port}{path}", data=data,
            method=method, headers=headers)
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, response.read().decode()
        except urllib.error.HTTPError as error:
            return error.code, error.read().decode()

    def raw(self, method, path, host=None, timeout=5):
        """Raw HTTP that does not follow redirects and lets the Host be set."""
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=timeout)
        headers = {}
        if host is not None:
            headers["Host"] = host
        conn.request(method, path, headers=headers)
        response = conn.getresponse()
        status = response.status
        location = response.getheader("Location")
        body = response.read().decode(errors="replace")
        conn.close()
        return status, location, body

    def network(self):
        status, text = self.http("GET", "/api/v1/network")
        assert status == 200, (status, text)
        return json.loads(text)["data"]

    def recovery(self):
        return self.network()["recovery"]

    def configure(self, enabled, auto_enabled, timeout_seconds):
        return self.http(
            "PUT", "/api/v1/network/recovery",
            json.dumps({"enabled": enabled, "auto_enabled": auto_enabled,
                        "timeout_seconds": timeout_seconds}))

    def prepare(self):
        return self.http("POST", "/api/v1/network/recovery/prepare", "")

    def close(self):
        if self.web is not None and self.web.poll() is None:
            self.web.terminate()
            try:
                self.web.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.web.kill()
        try:
            self.web_log.close()
        except OSError:
            pass
        self.fixture.stop()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


def test_configure_success_scales_timeout_seconds_to_ms():
    """PUT timeout_seconds=60 must reach networkd as auto_timeout_ms=60000."""
    with Harness() as h:
        assert h.recovery()["mode"] == "client"
        status, text = h.configure(True, True, 60)
        assert status == 200, (status, text)
        recovery = h.recovery()
        assert recovery["auto_timeout_ms"] == 60000, recovery
        assert recovery["enabled"] is True, recovery
        assert recovery["auto_enabled"] is True, recovery
        body = json.loads(text)["data"]["recovery"]
        assert body["auto_timeout_ms"] == 60000, body
        # Real networkd mode strings, never the mock-only "off" sentinel.
        network = h.network()
        assert network["mode"] in REAL_MODES, network["mode"]
        assert recovery["mode"] in REAL_MODES and recovery["mode"] != "off", recovery
        # The owner's choice was actually persisted under protected storage.
        config = WORKSPACE / "recovery.json"
        assert config.is_file(), "networkd did not persist the config"
        assert (config.stat().st_mode & 0o777) == 0o600, oct(config.stat().st_mode)
        # Bounds: 30 s and 600 s are the frozen floor and ceiling.
        for seconds, expected in ((30, 30000), (600, 600000)):
            status, text = h.configure(True, False, seconds)
            assert status == 200, (seconds, status, text)
            assert h.recovery()["auto_timeout_ms"] == expected, h.recovery()
        # Replacing the persisted config retains a mode-0600 backup of the
        # previous known-good version (AGENTS.md atomic write contract).
        backup = WORKSPACE / "recovery.json.bak"
        assert backup.is_file(), "no backup of the previous config"
        assert (backup.stat().st_mode & 0o777) == 0o600, oct(backup.stat().st_mode)
        # Out of range is refused by the API before the backend is reached.
        for seconds in (29, 601):
            status, _ = h.configure(True, False, seconds)
            assert status == 400, (seconds, status)


def test_prepare_success_chains_prepare_then_psk():
    """POST prepare runs recovery_prepare then recovery_psk over one socket."""
    with Harness() as h:
        assert h.recovery()["mode"] == "client"
        status, text = h.prepare()
        assert status == 200, (status, text)
        data = json.loads(text)["data"]
        assert data["ssid"].startswith("LibreEcho-Setup-"), data
        assert isinstance(data["psk"], str) and len(data["psk"]) == 32, data
        # The reveal is stable across calls (prepare retained, not regenerated).
        status, again = h.prepare()
        assert status == 200, (again,)
        assert json.loads(again)["data"]["psk"] == data["psk"]
        # The secret never leaks into read surfaces or logs.
        assert data["psk"] not in json.dumps(h.network())
        assert data["psk"] not in (WORKSPACE / "web-api.log").read_text(errors="replace")
        psk_file = WORKSPACE / "recovery-psk"
        assert psk_file.is_file() and (psk_file.stat().st_mode & 0o777) == 0o600


def test_captive_refusal_maps_to_conflict_not_server_error():
    """While the captive AP serves, the reveal is refused and maps to 409.

    Regression guard: the real backend used to collapse networkd's ok:false
    into LE_IO, so this returned 503/io_error while the API's own 409 branch
    and the mock backend both answered 409.
    """
    with Harness(marker=True) as h:
        lc.wait_for(lambda: h.recovery()["mode"] == "recovery-ap",
                    message="recovery AP active")
        # The captive AP is reported with networkd's real string, not a mock.
        network = h.network()
        assert network["mode"] == "recovery-ap", network
        assert network["recovery"]["mode"] == "recovery-ap", network["recovery"]
        status, text = h.prepare()
        assert status == 409, (status, text)
        error = json.loads(text)["error"]
        assert error["message"] == "Recovery password is unavailable in this mode", error
        assert error["code"] == "invalid_request", error
        # The refusal never exposes the secret.
        psk = (WORKSPACE / "recovery-psk").read_text().strip()
        assert len(psk) == 32 and psk not in text


def test_disabled_refusal_maps_to_conflict():
    """With the feature disabled networkd refuses the reveal: also 409."""
    with Harness() as h:
        status, text = h.configure(False, False, 60)
        assert status == 200, (status, text)
        assert h.recovery()["enabled"] is False
        status, text = h.prepare()
        assert status == 409, (status, text)
        assert json.loads(text)["error"]["message"] == \
            "Recovery password is unavailable in this mode"


def test_persist_failure_is_server_error_and_changes_nothing():
    """A real persist failure reaches the API as 503 and does not apply."""
    with Harness(config_blocker=True) as h:
        before = h.recovery()
        status, text = h.configure(True, True, 60)
        assert status == 503, (status, text)
        assert json.loads(text)["error"]["code"] == "io_error", text
        after = h.recovery()
        # The rejected write must not have mutated the running configuration.
        assert after["auto_timeout_ms"] == before["auto_timeout_ms"], (before, after)
        assert after["auto_enabled"] == before["auto_enabled"], (before, after)


def test_backend_unavailable_maps_to_not_supported():
    """A missing networkd is a 501, distinct from the 409/503 branches."""
    with Harness() as h:
        h.fixture.stop()
        deadline = time.monotonic() + 5
        while (WORKSPACE / "network.sock").exists() and time.monotonic() < deadline:
            time.sleep(0.02)
        status, text = h.prepare()
        assert status == 501, (status, text)
        assert json.loads(text)["error"]["code"] == "not_supported", text


def test_stop_succeeds_through_the_backend():
    with Harness(marker=True) as h:
        lc.wait_for(lambda: h.recovery()["mode"] == "recovery-ap",
                    message="recovery AP active")
        status, text = h.http("POST", "/api/v1/network/recovery/stop", "")
        assert status == 200, (status, text)
        lc.wait_for(lambda: h.recovery()["mode"] != "recovery-ap",
                    message="recovery stopped")


def test_stop_reports_failure_when_net_release_gave_up_through_the_backend():
    """The stop route maps a stuck net-down to a non-2xx, not 200.

    Codex review on d650caf: networkd now replies ok:false when every bounded
    net-down attempt has failed and no retry is scheduled; the API must surface
    that as a server error instead of claiming a still-owned interface was
    released.  The state stays truthful so the owner can retry.
    """
    with Harness(marker=True, net_down_fail_budget=3) as h:
        lc.wait_for(lambda: h.recovery()["mode"] == "recovery-ap",
                    message="recovery AP active")
        status, text = h.http("POST", "/api/v1/network/recovery/stop", "")
        assert status == 200, (status, text)
        lc.wait_for(
            lambda: h.fixture.read_net_down().count("--interface test0") >= 2,
            message="automatic net-down retry")
        status, text = h.http("POST", "/api/v1/network/recovery/stop", "")
        assert status == 503, (status, text)
        assert json.loads(text)["error"]["code"] == "io_error", text
        assert h.recovery()["net_configured"] is True


def test_recovery_ap_redirects_captive_probe_to_login():
    """While the recovery AP serves, a captive probe is sent to the login page.

    Regression (Codex review on 5660cc2): joining the recovery AP only ever
    produced the generic index (and only entered the recovery flow with an
    explicit ?recovery=1), so the owner had no way to reach sign-in by simply
    joining.  A probe path or a foreign Host now 302s to the recovery landing.
    """
    with Harness(marker=True) as h:
        lc.wait_for(lambda: h.recovery()["mode"] == "recovery-ap",
                    message="recovery AP active")
        status, location, body = h.raw("GET", "/generate_204",
                                       host="connectivitycheck.gstatic.com")
        assert status == 302, (status, location, body)
        assert location == "http://192.168.4.1/?recovery=1", location
        # A bare portal root (same host, no hint) is handed the recovery hint.
        status, location, body = h.raw("GET", "/", host="192.168.4.1")
        assert status == 302, (status, location, body)
        assert location == "http://192.168.4.1/?recovery=1", location


def test_recovery_ap_serves_recovery_landing():
    """The hinted portal root is served as setup.html (the sign-in landing)."""
    with Harness(marker=True) as h:
        lc.wait_for(lambda: h.recovery()["mode"] == "recovery-ap",
                    message="recovery AP active")
        status, location, body = h.raw("GET", "/?recovery=1", host="192.168.4.1")
        assert status == 200, (status, location, body)
        assert 'id="recovery-login"' in body, body[:200]


def test_normal_mode_has_no_captive_redirect():
    """Outside recovery the same requests are served normally, never 302'd."""
    with Harness() as h:
        assert h.recovery()["mode"] == "client"
        status, location, body = h.raw("GET", "/generate_204",
                                       host="connectivitycheck.gstatic.com")
        assert status != 302, (status, location, body)
        assert location is None, location
        status, location, body = h.raw("GET", "/", host="127.0.0.1")
        assert status == 200, (status, location, body)
        assert location is None, location


def main():
    tests = [
        test_configure_success_scales_timeout_seconds_to_ms,
        test_prepare_success_chains_prepare_then_psk,
        test_captive_refusal_maps_to_conflict_not_server_error,
        test_disabled_refusal_maps_to_conflict,
        test_persist_failure_is_server_error_and_changes_nothing,
        test_backend_unavailable_maps_to_not_supported,
        test_stop_succeeds_through_the_backend,
        test_stop_reports_failure_when_net_release_gave_up_through_the_backend,
        test_recovery_ap_redirects_captive_probe_to_login,
        test_recovery_ap_serves_recovery_landing,
        test_normal_mode_has_no_captive_redirect,
    ]
    failed = 0
    for test in tests:
        try:
            test()
            print(f"ok   {test.__name__}")
        except AssertionError as error:
            failed += 1
            print(f"FAIL {test.__name__}: {error}")
        except Exception as error:  # noqa: BLE001 - report and continue
            failed += 1
            print(f"ERROR {test.__name__}: {type(error).__name__}: {error}")
    if failed:
        print(f"recovery-backend-api-failures={failed}")
        return 1
    print(f"recovery-backend-api-ok={len(tests)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
