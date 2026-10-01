#!/usr/bin/env python3
"""Exercise the recovery access-point lifecycle through the real networkd.

The daemon is a real process and the softAP children are real fork/exec
children, but the "hostapd", "dnsmasq" and capability probes are isolated
shell oracles in a temp directory.  Nothing touches a real radio, network
interface, DHCP server or the host network.
"""

import json
import os
from pathlib import Path
import signal
import socket
import stat
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/test-networkd-recovery"
MARKER_TAG = "libreecho-recovery-v1"


class FakeWpa:
    """Minimal wpa_supplicant control oracle."""

    def __init__(self, path, association_fails=False, saved_network=True):
        self.path = path
        self.association_fails = association_fails
        self.saved_network = saved_network
        self.connected = False
        self.network_id = 0
        self.next_network_id = 1
        self.monitor_addr = None
        self.commands = []
        self.stop_event = threading.Event()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind(str(path))
        self.sock.settimeout(0.05)
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while not self.stop_event.is_set():
            try:
                payload, peer = self.sock.recvfrom(8192)
            except socket.timeout:
                continue
            except OSError:
                return
            command = payload.decode("utf-8", "replace").strip()
            self.commands.append(command)
            if command == "ATTACH":
                self.monitor_addr = peer
                response = "OK\n"
            elif command == "STATUS":
                state = "COMPLETED" if self.connected else "DISCONNECTED"
                response = f"wpa_state={state}\nssid=HomeNet\n"
                if self.connected:
                    response += f"id={self.network_id}\n"
            elif command == "SIGNAL_POLL":
                response = "RSSI=-50\n"
            elif command == "ADD_NETWORK":
                response = f"{self.next_network_id}\n"
                self.next_network_id += 1
            elif command == "LIST_NETWORKS":
                header = "network id / ssid / bssid / flags\n"
                if self.saved_network:
                    response = header + "0\tHomeNet\tany\t[CURRENT]\n"
                else:
                    response = header
            elif command == "DISCONNECT":
                self.connected = False
                response = "OK\n"
            elif command.startswith("SELECT_NETWORK"):
                self.network_id = int(command.split()[1])
                # The oracle associates unless the fixture is explicitly told
                # the credentials are wrong; it does not depend on the id.
                self.connected = not self.association_fails
                response = "OK\n"
            elif command == "SCAN":
                response = "OK\n"
            elif command == "SCAN_RESULTS":
                response = "bssid / frequency / signal level / flags / ssid\n"
            else:
                response = "OK\n"
            try:
                self.sock.sendto(response.encode(), peer)
            except OSError:
                pass

    def close(self):
        self.stop_event.set()
        try:
            self.sock.close()
        except OSError:
            pass
        self.thread.join(timeout=1)
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass


class FakeLed:
    """ledd oracle recording pattern ownership over the adapter wire format."""

    def __init__(self, path):
        self.requests = []
        self.stop_event = threading.Event()
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(str(path))
        self.sock.listen(8)
        self.sock.settimeout(0.05)
        self.thread = threading.Thread(target=self._serve, daemon=True)
        self.thread.start()

    def _serve(self):
        while not self.stop_event.is_set():
            try:
                conn, _ = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            with conn:
                conn.settimeout(0.5)
                try:
                    payload = conn.recv(4096).decode("utf-8", "replace")
                except OSError:
                    continue
                for line in payload.splitlines():
                    if not line.strip():
                        continue
                    try:
                        request = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if request.get("cmd") == "pattern":
                        self.requests.append(request.get("args", {}))
                try:
                    conn.sendall(b'{"v":1,"id":1,"ok":true,"data":{}}\n')
                except OSError:
                    pass

    def pattern_owners(self):
        return [(item.get("owner"), item.get("name")) for item in self.requests]

    def close(self):
        self.stop_event.set()
        try:
            self.sock.close()
        except OSError:
            pass
        self.thread.join(timeout=1)
        try:
            Path(self.sock.getsockname()).unlink()
        except (FileNotFoundError, OSError):
            pass


def wait_for(predicate, timeout=5.0, message="condition"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.01)
    raise AssertionError(f"timed out waiting for {message}")


def write_script(directory, name, body):
    path = directory / name
    path.write_text("#!/bin/sh\n" + body)
    path.chmod(0o755)
    return path


def adapter_request(path, request_id, command, args=None, timeout=3):
    request = {"v": 1, "id": request_id, "cmd": command,
               "args": args if args is not None else {}}
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(timeout)
    client.connect(str(path))
    client.sendall((json.dumps(request) + "\n").encode())
    with client:
        reader = client.makefile("r", encoding="utf-8")
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"no adapter response for {command}")
            client.settimeout(remaining)
            line = reader.readline()
            if not line:
                raise AssertionError(f"connection closed for {command}")
            response = json.loads(line)
            if response.get("id") == request_id:
                return response


def pid_alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    # A zombie is not alive for our purposes.
    try:
        state = Path(f"/proc/{pid}/stat").read_text().split(") ", 1)[1][0]
        return state != "Z"
    except (FileNotFoundError, IndexError):
        return False


class RecoveryFixture:
    def __init__(self, directory, *, marker=True, marker_kind="valid",
                 auto=False, auto_timeout_ms=400, ap_probe=True,
                 association_fails=False, saved_network=True,
                 hostapd_hang=False, start_timeout_ms=300,
                 stop_timeout_ms=400):
        self.directory = directory
        self.wpa = FakeWpa(directory / "wpa.sock",
                           association_fails=association_fails,
                           saved_network=saved_network)
        self.led = FakeLed(directory / "led.sock")
        self.marker_path = directory / "recovery-mode"
        self.psk_path = directory / "recovery-psk"
        self.config_path = directory / "recovery.json"
        self.run_dir = directory / "run"
        self.run_dir.mkdir(exist_ok=True)
        self.log = directory / "daemon.log"
        self.oracle_log = directory / "oracle.log"
        self.hostapd_conf = directory / "hostapd.conf"
        self.dns_conf = directory / "dnsmasq.conf"
        self.hostapd_pid_file = directory / "hostapd.pid"
        self.dns_pid_file = directory / "dns.pid"
        self.net_up_log = directory / "net-up.log"
        self.net_down_log = directory / "net-down.log"

        self.hostapd = write_script(
            directory, "hostapd.sh",
            f'echo $$ > "{self.hostapd_pid_file}"\n'
            f'echo "hostapd-started" >> "{self.oracle_log}"\n'
            "exec sleep 3600\n")
        self.dnsmasq = write_script(
            directory, "dnsmasq.sh",
            f'echo $$ > "{self.dns_pid_file}"\n'
            f'echo "dnsmasq-started" >> "{self.oracle_log}"\n'
            "exec sleep 3600\n")
        if hostapd_hang:
            self.hostapd = write_script(
                directory, "hostapd-hang.sh",
                f'echo $$ > "{self.hostapd_pid_file}"\n'
                "trap '' TERM\n"
                "while :; do sleep 1; done\n")
        self.ap_probe = write_script(directory, "ap-probe.sh", "exit 0\n")
        if not ap_probe:
            self.ap_probe = None
        self.ready_probe = write_script(directory, "ready-probe.sh", "exit 0\n")
        # Interface helpers: record the exact argument vector the daemon uses.
        # No real interface is touched.
        self.net_up = write_script(
            directory, "net-up.sh",
            f'echo "$@" >> "{self.net_up_log}"\nexit 0\n')
        self.net_down = write_script(
            directory, "net-down.sh",
            f'echo "$@" >> "{self.net_down_log}"\nexit 0\n')

        if marker:
            if marker_kind == "valid":
                self.marker_path.write_text(MARKER_TAG + "\nhold_ms=5000\n")
                self.marker_path.chmod(0o600)
            elif marker_kind == "symlink":
                target = directory / "marker-target"
                target.write_text(MARKER_TAG + "\n")
                os.symlink(target, self.marker_path)
            elif marker_kind == "world-writable":
                self.marker_path.write_text(MARKER_TAG + "\n")
                self.marker_path.chmod(0o666)
            elif marker_kind == "bad-content":
                self.marker_path.write_text("not-a-recovery-marker\n")
                self.marker_path.chmod(0o600)

        env = os.environ.copy()
        env.update({
            "LIBREECHO_NETWORKD_TEST_FIXTURE": "1",
            "LIBREECHO_RECOVERY_TEST_MARKER_RELAX": "1",
        })
        args = [
            str(BINARY), "--foreground", "--quiet",
            "--socket", str(directory / "network.sock"),
            "--wpa-ctrl", str(self.wpa.path),
            "--interface", "test0",
            "--reboot-request", str(directory / "reboot.request"),
            "--reboot-guard", str(directory / "reboot.guard"),
            "--recovery-marker", str(self.marker_path),
            "--recovery-psk", str(self.psk_path),
            "--recovery-config", str(self.config_path),
            "--recovery-run-dir", str(self.run_dir),
            "--recovery-timeout", str(auto_timeout_ms),
            "--recovery-start-timeout", str(start_timeout_ms),
            "--recovery-stop-timeout", str(stop_timeout_ms),
            "--hostapd", str(self.hostapd),
            "--hostapd-conf", str(self.hostapd_conf),
            "--recovery-dhcp", str(self.dnsmasq),
            "--recovery-dns", str(self.dnsmasq),
            "--recovery-conf", str(self.dns_conf),
            "--led-socket", str(directory / "led.sock"),
        ]
        if self.ap_probe is not None:
            args += ["--recovery-ap-probe", str(self.ap_probe)]
        args += ["--recovery-ready-probe", str(self.ready_probe)]
        args += ["--recovery-net-up", str(self.net_up),
                 "--recovery-net-down", str(self.net_down),
                 "--recovery-address", "192.168.4.1"]
        if auto:
            args.append("--recovery-auto")
        self.env = env
        self.args = args
        self.log_handle = None
        self.process = None
        self.adapter = directory / "network.sock"
        self._start()

    def _start(self):
        log = open(self.log, "ab")
        self.log_handle = log
        self.process = subprocess.Popen(
            self.args, cwd=ROOT, env=self.env, stdin=subprocess.DEVNULL,
            stdout=log, stderr=log)
        wait_for(lambda: self.adapter.exists(), message="networkd socket")

    def restart(self):
        """Stop and relaunch the daemon with the same paths, simulating a reboot
        so persisted recovery state (password + owner configuration) is reloaded
        from disk rather than memory."""
        proc = self.process
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
        handle = self.log_handle
        if handle is not None:
            try:
                handle.close()
            except OSError:
                pass
            self.log_handle = None
        try:
            self.adapter.unlink()
        except FileNotFoundError:
            pass
        self._start()

    def status(self):
        return adapter_request(self.adapter, 1, "status")["data"]

    def recovery(self):
        return self.status()["recovery"]

    def stop(self):
        proc = self.process
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=2)
        handle = self.log_handle
        if handle is not None:
            try:
                handle.close()
            except OSError:
                pass
        self.wpa.close()
        self.led.close()

    def read_log(self):
        return self.log.read_text(errors="replace")

    def read_oracle(self):
        if not self.oracle_log.exists():
            return ""
        return self.oracle_log.read_text(errors="replace")

    def read_net_up(self):
        return self.net_up_log.read_text(errors="replace") \
            if self.net_up_log.exists() else ""

    def read_net_down(self):
        return self.net_down_log.read_text(errors="replace") \
            if self.net_down_log.exists() else ""


def test_physical_entry_starts_ap():
    with tempfile.TemporaryDirectory(prefix="le-recovery-physical-") as temp:
        fixture = RecoveryFixture(Path(temp))
        try:
            wait_for(lambda: fixture.recovery()["mode"] == "recovery-ap",
                     message="recovery AP active")
            status = fixture.status()
            assert status["mode"] == "recovery-ap"
            recovery = status["recovery"]
            assert recovery["trigger"] == "physical"
            assert recovery["secret_available"] is True
            assert recovery["led_owner"] == "recovery-ap"
            assert recovery["ssid"].startswith("LibreEcho-Setup-")
            assert recovery["children"] == 2
            # Both oracle children really ran.
            assert "hostapd-started" in fixture.read_oracle()
            assert "dnsmasq-started" in fixture.read_oracle()
            # The per-device softAP config was generated 0600 with the random
            # passphrase, and the DHCP/DNS config only serves the portal.
            conf = fixture.hostapd_conf.read_text()
            assert f"ssid={recovery['ssid']}" in conf
            assert "wpa_passphrase=" in conf
            assert stat.S_IMODE(fixture.hostapd_conf.stat().st_mode) == 0o600
            dns_conf = fixture.dns_conf.read_text()
            assert "dhcp-range=192.168.4.10,192.168.4.200" in dns_conf
            assert "address=/#/192.168.4.1" in dns_conf
            assert "users" not in dns_conf
            # The platform interface helper assigned the portal address before
            # any child started, so dnsmasq can bind and lease.
            assert recovery["net_configured"] is True
            assert "--interface test0" in fixture.read_net_up()
            assert "--address 192.168.4.1/24" in fixture.read_net_up()
            # The LED owner was taken through the existing pattern protocol.
            wait_for(lambda: ("recovery-ap", "pulse")
                     in fixture.led.pattern_owners(),
                     message="recovery LED ownership")
        finally:
            fixture.stop()


def test_normal_boot_never_enters_recovery():
    with tempfile.TemporaryDirectory(prefix="le-recovery-normal-") as temp:
        fixture = RecoveryFixture(Path(temp), marker=False)
        try:
            time.sleep(0.6)
            assert fixture.status()["mode"] == "client"
            assert not fixture.hostapd_pid_file.exists()
        finally:
            fixture.stop()


def test_rejected_markers_never_enter_recovery():
    for kind in ("symlink", "world-writable", "bad-content"):
        with tempfile.TemporaryDirectory(
                prefix=f"le-recovery-{kind}-") as temp:
            fixture = RecoveryFixture(Path(temp), marker_kind=kind)
            try:
                time.sleep(0.5)
                assert fixture.recovery()["mode"] == "unavailable" or \
                    fixture.status()["mode"] == "client", kind
                assert not fixture.hostapd_pid_file.exists(), kind
            finally:
                fixture.stop()


def test_capability_failure_fails_closed():
    with tempfile.TemporaryDirectory(prefix="le-recovery-cap-") as temp:
        fixture = RecoveryFixture(Path(temp), ap_probe=False)
        try:
            wait_for(lambda: fixture.status()["mode"] == "unavailable",
                     message="unavailable without AP driver probe")
            recovery = fixture.recovery()
            assert recovery["available"] is False
            assert recovery["reason"] == "ap-driver-unverified"
            assert not fixture.hostapd_pid_file.exists()
        finally:
            fixture.stop()


def test_invalid_credentials_keep_ap_and_surface_error():
    with tempfile.TemporaryDirectory(prefix="le-recovery-badcreds-") as temp:
        fixture = RecoveryFixture(Path(temp), association_fails=True)
        try:
            wait_for(lambda: fixture.recovery()["mode"] == "recovery-ap",
                     message="recovery AP active")
            response = adapter_request(
                fixture.adapter, 7, "connect",
                {"ssid": "WrongNet", "psk": "hunter2hunter2",
                 "security": "wpa2"}, timeout=6)
            assert response["ok"] is False, response
            assert "association did not complete" in response["error"]
            # Single radio: the AP released the interface for the attempt
            # (net-down) and was rebuilt afterwards, keeping the marker.
            assert "--interface test0" in fixture.read_net_down()
            wait_for(lambda: fixture.recovery()["mode"] == "recovery-ap",
                     timeout=5, message="AP rebuilt after failed attempt")
            assert fixture.marker_path.exists()
            wait_for(lambda: fixture.hostapd_pid_file.exists() and
                     int(fixture.hostapd_pid_file.read_text().strip()),
                     message="hostapd rebuilt")
            assert pid_alive(int(fixture.hostapd_pid_file.read_text().strip()))
            assert fixture.recovery()["net_configured"] is True
            assert fixture.read_net_up().count("--address") >= 2
        finally:
            fixture.stop()


def test_successful_association_ends_recovery():
    with tempfile.TemporaryDirectory(prefix="le-recovery-success-") as temp:
        fixture = RecoveryFixture(Path(temp), association_fails=False)
        try:
            wait_for(lambda: fixture.recovery()["mode"] == "recovery-ap",
                     message="recovery AP active")
            first_hostapd = int(fixture.hostapd_pid_file.read_text().strip())
            response = adapter_request(
                fixture.adapter, 11, "connect",
                {"ssid": "HomeNet", "psk": "correcthorsebattery",
                 "security": "wpa2"}, timeout=6)
            # DHCP cannot run in the fixture, so the command reports an error,
            # but the recovery state must already have ended: marker removed,
            # AP down and interface released.
            assert response["ok"] is False, response
            assert not fixture.marker_path.exists()
            wait_for(lambda: fixture.recovery()["mode"] != "recovery-ap",
                     message="recovery ended")
            assert "--interface test0" in fixture.read_net_down()
            wait_for(lambda: not pid_alive(first_hostapd),
                     message="AP hostapd torn down on success")
        finally:
            fixture.stop()


def test_owner_stop_releases_ap_marker_and_led():
    with tempfile.TemporaryDirectory(prefix="le-recovery-stop-") as temp:
        fixture = RecoveryFixture(Path(temp))
        try:
            wait_for(lambda: fixture.recovery()["mode"] == "recovery-ap",
                     message="recovery AP active")
            hostapd_pid = int(fixture.hostapd_pid_file.read_text().strip())
            dns_pid = int(fixture.dns_pid_file.read_text().strip())
            response = adapter_request(fixture.adapter, 3, "recovery_stop",
                                       timeout=4)
            assert response["ok"] is True
            assert not fixture.marker_path.exists()
            wait_for(lambda: not pid_alive(hostapd_pid),
                     message="hostapd child reaped")
            wait_for(lambda: not pid_alive(dns_pid),
                     message="dnsmasq child reaped")
            assert (("recovery-ap", "stop") in fixture.led.pattern_owners())
            # The portal address was removed on teardown.
            assert "--interface test0" in fixture.read_net_down()
            assert fixture.recovery()["net_configured"] is False
        finally:
            fixture.stop()


def test_hung_child_is_killed_within_bound():
    with tempfile.TemporaryDirectory(prefix="le-recovery-hang-") as temp:
        fixture = RecoveryFixture(Path(temp), hostapd_hang=True,
                                  stop_timeout_ms=300)
        try:
            wait_for(lambda: fixture.recovery()["mode"] == "recovery-ap",
                     message="recovery AP active")
            hostapd_pid = int(fixture.hostapd_pid_file.read_text().strip())
            started = time.monotonic()
            response = adapter_request(fixture.adapter, 4, "recovery_stop",
                                       timeout=4)
            elapsed = time.monotonic() - started
            assert response["ok"] is True
            assert elapsed < 3.0, elapsed
            assert not pid_alive(hostapd_pid)
        finally:
            fixture.stop()


def test_owner_saves_password_before_recovery():
    """A client-connected owner prepares/reveals the password in advance.

    The protected AP cannot be joined without the password, so this is the only
    point at which the owner can save it.
    """
    with tempfile.TemporaryDirectory(prefix="le-recovery-save-") as temp:
        fixture = RecoveryFixture(Path(temp), marker=False)
        try:
            assert fixture.status()["mode"] == "client"
            prepared = adapter_request(fixture.adapter, 20, "recovery_prepare")
            assert prepared["ok"] is True, prepared
            secret = adapter_request(fixture.adapter, 21,
                                     "recovery_psk")["data"]
            assert len(secret["psk"]) == 32
            assert secret["ssid"].startswith("LibreEcho-Setup-")
            mode = stat.S_IMODE(fixture.psk_path.stat().st_mode)
            assert mode == 0o600, oct(mode)
            assert secret["psk"] not in json.dumps(fixture.status())
            assert secret["psk"] not in fixture.read_log()
            again = adapter_request(fixture.adapter, 22,
                                    "recovery_psk")["data"]
            assert again["psk"] == secret["psk"]
        finally:
            fixture.stop()


def test_secret_refused_while_captive_ap_active():
    with tempfile.TemporaryDirectory(prefix="le-recovery-secret-") as temp:
        fixture = RecoveryFixture(Path(temp))
        try:
            wait_for(lambda: fixture.recovery()["mode"] == "recovery-ap",
                     message="recovery AP active")
            psk = fixture.psk_path.read_text().strip()
            assert len(psk) == 32
            mode = stat.S_IMODE(fixture.psk_path.stat().st_mode)
            assert mode == 0o600, oct(mode)
            status_text = json.dumps(fixture.status())
            assert psk not in status_text
            recovery_text = json.dumps(
                adapter_request(fixture.adapter, 6, "recovery_status")["data"])
            assert psk not in recovery_text
            # While the AP is serving, an unauthenticated captive client cannot
            # obtain or prepare the secret.
            refused = adapter_request(fixture.adapter, 8, "recovery_psk")
            assert refused["ok"] is False, refused
            prepared = adapter_request(fixture.adapter, 9, "recovery_prepare")
            assert prepared["ok"] is False, prepared
            assert psk not in fixture.read_log()
        finally:
            fixture.stop()


def test_auto_fallback_is_opt_in_and_delayed():
    # Default (no --recovery-auto): never arms even with a saved network.
    with tempfile.TemporaryDirectory(prefix="le-recovery-auto-off-") as temp:
        fixture = RecoveryFixture(Path(temp), marker=False, auto=False,
                                  auto_timeout_ms=300)
        try:
            time.sleep(0.8)
            assert fixture.status()["mode"] == "client"
            assert fixture.recovery()["auto_enabled"] is False
        finally:
            fixture.stop()

    # Opted in: the AP only appears after the bounded window.
    with tempfile.TemporaryDirectory(prefix="le-recovery-auto-on-") as temp:
        fixture = RecoveryFixture(Path(temp), marker=False, auto=True,
                                  auto_timeout_ms=600)
        try:
            time.sleep(0.2)
            assert fixture.status()["mode"] == "client"
            wait_for(lambda: fixture.recovery()["mode"] == "recovery-ap",
                     timeout=4, message="opt-in auto fallback")
            assert fixture.recovery()["trigger"] == "auto"
        finally:
            fixture.stop()

    # Opted in but never provisioned: the first-boot AP owns that case.
    with tempfile.TemporaryDirectory(prefix="le-recovery-auto-new-") as temp:
        fixture = RecoveryFixture(Path(temp), marker=False, auto=True,
                                  saved_network=False, auto_timeout_ms=300)
        try:
            time.sleep(0.9)
            assert fixture.status()["mode"] == "client"
            assert not fixture.hostapd_pid_file.exists()
        finally:
            fixture.stop()


def test_children_cleaned_up_on_shutdown():
    with tempfile.TemporaryDirectory(prefix="le-recovery-shutdown-") as temp:
        fixture = RecoveryFixture(Path(temp))
        try:
            wait_for(lambda: fixture.recovery()["mode"] == "recovery-ap",
                     message="recovery AP active")
            hostapd_pid = int(fixture.hostapd_pid_file.read_text().strip())
            dns_pid = int(fixture.dns_pid_file.read_text().strip())
            fixture.process.terminate()
            fixture.process.wait(timeout=4)
            wait_for(lambda: not pid_alive(hostapd_pid),
                     message="hostapd reaped on shutdown")
            wait_for(lambda: not pid_alive(dns_pid),
                     message="dnsmasq reaped on shutdown")
        finally:
            fixture.stop()


def test_configure_persists_and_survives_restart():
    """Owner configuration is persisted under protected storage and reloaded.

    The password path and the owner enable/auto choices must survive a reboot:
    the daemon reloads them before it evaluates the boot trigger.
    """
    with tempfile.TemporaryDirectory(prefix="le-recovery-config-") as temp:
        fixture = RecoveryFixture(Path(temp), marker=False)
        try:
            assert fixture.status()["mode"] == "client"
            response = adapter_request(
                fixture.adapter, 30, "recovery_configure",
                {"enabled": True, "auto_enabled": True,
                 "auto_timeout_ms": 60000})
            assert response["ok"] is True, response
            recovery = fixture.recovery()
            assert recovery["enabled"] is True
            assert recovery["auto_enabled"] is True
            assert recovery["auto_timeout_ms"] == 60000
            assert "psk_path" not in recovery
            config = fixture.config_path
            assert config.exists()
            assert stat.S_IMODE(config.stat().st_mode) == 0o600

            # Reboot: the owner choice is reloaded from disk, not memory.
            fixture.restart()
            recovery = fixture.recovery()
            assert recovery["enabled"] is True, recovery
            assert recovery["auto_enabled"] is True, recovery
            assert recovery["auto_timeout_ms"] == 60000, recovery
            time.sleep(0.2)
            # A 60 s window has not elapsed, so the AP stays down.
            assert fixture.status()["mode"] == "client"
            assert not fixture.hostapd_pid_file.exists()
        finally:
            fixture.stop()


def test_configure_rejects_out_of_range_and_malformed():
    with tempfile.TemporaryDirectory(prefix="le-recovery-config-bad-") as temp:
        fixture = RecoveryFixture(Path(temp), marker=False)
        try:
            before = fixture.recovery()["auto_timeout_ms"]
            # Below the 30 s floor: rejected, never silently clamped.
            response = adapter_request(
                fixture.adapter, 31, "recovery_configure",
                {"enabled": True, "auto_enabled": False,
                 "auto_timeout_ms": 1000})
            assert response["ok"] is False, response
            assert "auto-timeout-range" in response["error"], response
            # Above the 600 s ceiling.
            response = adapter_request(
                fixture.adapter, 32, "recovery_configure",
                {"enabled": True, "auto_enabled": False,
                 "auto_timeout_ms": 600001})
            assert response["ok"] is False, response
            assert "auto-timeout-range" in response["error"], response
            # A non-boolean field is rejected.
            response = adapter_request(
                fixture.adapter, 33, "recovery_configure",
                {"enabled": "yes", "auto_enabled": False,
                 "auto_timeout_ms": 60000})
            assert response["ok"] is False, response
            # The running configuration is unchanged and nothing was persisted.
            recovery = fixture.recovery()
            assert recovery["auto_timeout_ms"] == before, recovery
            assert not fixture.config_path.exists()
        finally:
            fixture.stop()


def test_disabled_config_blocks_boot_and_auto():
    """An owner-disabled feature stays disabled across reboot for both entries."""
    with tempfile.TemporaryDirectory(prefix="le-recovery-disabled-") as temp:
        fixture = RecoveryFixture(Path(temp), marker=False, auto=False,
                                  auto_timeout_ms=30000)
        try:
            response = adapter_request(
                fixture.adapter, 40, "recovery_configure",
                {"enabled": False, "auto_enabled": True,
                 "auto_timeout_ms": 30000})
            assert response["ok"] is True, response
            assert fixture.recovery()["enabled"] is False

            # A valid physical marker plus an opt-in auto, then a reboot: the
            # persisted owner choice must block both entries.
            fixture.marker_path.write_text(MARKER_TAG + "\nhold_ms=5000\n")
            fixture.marker_path.chmod(0o600)
            fixture.restart()
            recovery = fixture.recovery()
            assert recovery["enabled"] is False, recovery
            time.sleep(0.8)
            assert fixture.status()["mode"] == "client"
            assert not fixture.hostapd_pid_file.exists()
        finally:
            fixture.stop()


def main():
    tests = [
        test_physical_entry_starts_ap,
        test_normal_boot_never_enters_recovery,
        test_rejected_markers_never_enter_recovery,
        test_capability_failure_fails_closed,
        test_invalid_credentials_keep_ap_and_surface_error,
        test_successful_association_ends_recovery,
        test_owner_stop_releases_ap_marker_and_led,
        test_hung_child_is_killed_within_bound,
        test_owner_saves_password_before_recovery,
        test_secret_refused_while_captive_ap_active,
        test_auto_fallback_is_opt_in_and_delayed,
        test_children_cleaned_up_on_shutdown,
        test_configure_persists_and_survives_restart,
        test_configure_rejects_out_of_range_and_malformed,
        test_disabled_config_blocks_boot_and_auto,
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
        print(f"recovery-lifecycle-failures={failed}")
        return 1
    print(f"recovery-lifecycle-ok={len(tests)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
