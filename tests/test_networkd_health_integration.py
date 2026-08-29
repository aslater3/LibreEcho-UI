#!/usr/bin/env python3
"""Exercise networkd recovery through its real poll loop."""

import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / "build/test-networkd-health"


class FakeWpa:
    def __init__(self, path: Path, fail_reassociate: bool = False,
                 association_fails: bool = False, scan_results: str = ""):
        self.path = path
        self.fail_reassociate = fail_reassociate
        self.association_fails = association_fails
        self.scan_results = scan_results
        self.connected = True
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
                response = (f"wpa_state={state}\nssid=IntegrationNet\n"
                            f"id={self.network_id}\n")
            elif command == "SIGNAL_POLL":
                response = "RSSI=-45\n"
            elif command == "ADD_NETWORK":
                response = f"{self.next_network_id}\n"
                self.next_network_id += 1
            elif command == "DISCONNECT":
                self.connected = False
                response = "OK\n"
            elif command.startswith("SELECT_NETWORK"):
                self.network_id = int(command.split()[1])
                self.connected = self.network_id == 0 or not self.association_fails
                response = "OK\n"
            elif command.startswith("REMOVE_NETWORK"):
                response = "OK\n"
            elif command == "SCAN":
                response = "OK\n"
            elif command == "SCAN_RESULTS":
                response = self.scan_results or (
                    "bssid / frequency / signal level / flags / ssid\n")
            elif command == "REASSOCIATE" and self.fail_reassociate:
                response = "FAIL\n"
            else:
                response = "OK\n"
            try:
                self.sock.sendto(response.encode(), peer)
                if command == "SCAN" and self.monitor_addr:
                    self.sock.sendto(b"CTRL-EVENT-SCAN-RESULTS\n",
                                     self.monitor_addr)
            except OSError:
                pass

    def close(self):
        self.stop_event.set()
        self.sock.close()
        self.thread.join(timeout=1)
        try:
            self.path.unlink()
        except FileNotFoundError:
            pass


def wait_for(predicate, timeout=3.0, message="condition"):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.005)
    raise AssertionError(f"timed out waiting for {message}")


def read_actions(path: Path):
    if not path.exists():
        return []
    return [line for line in path.read_text().splitlines() if line]


def queue_adapter_request(path: Path, request_id: int, command: str,
                          args=None, timeout=2):
    request = {"v": 1, "id": request_id, "cmd": command,
               "args": args if args is not None else {}}
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(timeout)
    client.connect(str(path))
    client.sendall((json.dumps(request) + "\n").encode())
    return client


def read_adapter_response(client, request_id: int):
    reader = client.makefile("r", encoding="utf-8")
    seen = []
    while True:
        try:
            line = reader.readline()
        except TimeoutError as error:
            raise AssertionError(
                f"adapter response timeout; received={seen!r}") from error
        response = json.loads(line)
        seen.append(response)
        if response.get("id") == request_id:
            return response


def adapter_request(path: Path, request_id: int, command: str, args=None,
                    timeout=2):
    with queue_adapter_request(path, request_id, command, args,
                               timeout=timeout) as client:
        return read_adapter_response(client, request_id)


def start_daemon(directory: Path, script: str, *, fail_reassociate=False,
                 fail_interface=None, invalid_reboot_path=False,
                 barrier_at=None, preexisting_reboot=None,
                 preexisting_guard=False, reboot_fifo=False,
                 association_fails=False, scan_results=""):
    wpa_path = directory / "wpa.sock"
    adapter_path = directory / "network.sock"
    action_log = directory / "actions.log"
    reboot_path = (Path("/proc/libreecho-networkd-test/reboot.request")
                   if invalid_reboot_path else directory / "reboot.request")
    guard_path = directory / "network-recovery-reboot.guard"
    if preexisting_reboot is not None:
        reboot_path.write_text(preexisting_reboot)
    if reboot_fifo:
        os.mkfifo(reboot_path)
    if preexisting_guard:
        guard_path.write_text("network-reboot-v1\n")
    wpa = FakeWpa(wpa_path, fail_reassociate=fail_reassociate,
                  association_fails=association_fails,
                  scan_results=scan_results)
    env = os.environ.copy()
    env.update({
        "LIBREECHO_NETWORKD_TEST_FIXTURE": "1",
        "LIBREECHO_GATEWAY_PROBE_SCRIPT": script,
        "LIBREECHO_NETWORKD_TEST_ACTION_LOG": str(action_log),
    })
    if fail_interface:
        env["LIBREECHO_NETWORKD_TEST_FAIL_INTERFACE"] = fail_interface
    if barrier_at is not None:
        env["LIBREECHO_GATEWAY_PROBE_BARRIER"] = str(
            directory / "probe-barrier")
        env["LIBREECHO_GATEWAY_PROBE_BARRIER_AT"] = str(barrier_at)
    process = subprocess.Popen(
        [str(BINARY), "--foreground", "--quiet",
         "--socket", str(adapter_path), "--wpa-ctrl", str(wpa_path),
         "--interface", "test0", "--reboot-request", str(reboot_path),
         "--reboot-guard", str(guard_path)],
        cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    wait_for(lambda: adapter_path.exists(), message="networkd socket")
    return process, wpa, adapter_path, action_log, reboot_path


def stop_daemon(process, wpa):
    process.terminate()
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)
    wpa.close()


def test_ordered_recovery_and_one_shot_reboot():
    with tempfile.TemporaryDirectory(prefix="libreecho-networkd-order-") as temp:
        directory = Path(temp)
        process, wpa, adapter, actions, reboot = start_daemon(
            directory, "1," + ",".join("0" for _ in range(16)))
        try:
            wait_for(reboot.exists, message="supervised reboot request")
            expected = ["reassociate", "interface-down", "interface-up",
                        "reboot-request"]
            assert read_actions(actions) == expected, read_actions(actions)
            assert reboot.read_text() == "reboot\n"
            assert (directory / "network-recovery-reboot.guard").read_text() == \
                "network-reboot-v1\n"
            time.sleep(0.15)
            assert read_actions(actions) == expected
            status = adapter_request(adapter, 1, "status")
            assert status["ok"] is True
            assert status["data"]["state"] == "connected"
            assert status["data"]["connectivity"] == "recovering"
            assert status["data"]["recovery_stage"] == "reboot-requested"
            assert wpa.commands.count("REASSOCIATE") == 2
        finally:
            stop_daemon(process, wpa)


def test_recovery_can_succeed_without_supervisor_request():
    with tempfile.TemporaryDirectory(prefix="libreecho-networkd-recover-") as temp:
        directory = Path(temp)
        # The first post-reassociation probe still fails, so networkd performs
        # its single interface cycle.  The following probe succeeds.
        script = "1,0,0,0,0,1,1"
        proc, wpa, adapter, action_log, reboot = start_daemon(directory, script)
        try:
            wait_for(lambda: read_actions(action_log) ==
                     ["reassociate", "interface-down", "interface-up"],
                     message="bounded pre-reboot recovery sequence")
            wait_for(lambda: adapter_request(adapter, 9, "status")["data"]
                     ["connectivity"] == "healthy",
                     message="healthy state after interface recovery")
            time.sleep(0.15)
            assert read_actions(action_log) == [
                "reassociate", "interface-down", "interface-up"]
            assert not reboot.exists()
            assert wpa.commands.count("REASSOCIATE") == 2
        finally:
            stop_daemon(proc, wpa)


def test_explicit_commands_cancel_pending_recovery():
    for command in ("disconnect", "connect"):
        with tempfile.TemporaryDirectory(
                prefix=f"libreecho-networkd-{command}-") as temp:
            directory = Path(temp)
            barrier = directory / "probe-barrier"
            process, wpa, adapter, actions, reboot = start_daemon(
                directory, "1,0,0,0,0,1," +
                ",".join("0" for _ in range(32)), barrier_at=6)
            try:
                wait_for(barrier.exists,
                         message="simultaneous client/probe barrier")
                args = None if command == "disconnect" else {
                    "ssid": "ReplacementNet", "psk": "", "security": "open"}
                client = queue_adapter_request(adapter, 2, command, args)
                try:
                    Path(f"{barrier}.release").touch()
                    response = read_adapter_response(client, 2)
                finally:
                    client.close()
                assert response["id"] == 2
                time.sleep(0.15)
                assert read_actions(actions) == [
                    "reassociate", "interface-down", "interface-up"]
                assert not reboot.exists(), (command, read_actions(actions))
            finally:
                stop_daemon(process, wpa)


def test_persistent_reboot_guard_blocks_cross_boot_loop():
    with tempfile.TemporaryDirectory(prefix="libreecho-networkd-guard-") as temp:
        directory = Path(temp)
        process, wpa, adapter, actions, reboot = start_daemon(
            directory, "1," + ",".join("0" for _ in range(32)),
            preexisting_guard=True)
        try:
            wait_for(lambda: read_actions(actions)[-1:] == ["reboot-request"],
                     message="guarded reboot escalation")
            time.sleep(0.1)
            assert not reboot.exists()
            status = adapter_request(adapter, 4, "status")
            assert status["data"]["recovery_stage"] == "exhausted"
        finally:
            stop_daemon(process, wpa)


def test_stale_non_reboot_request_is_not_accepted():
    with tempfile.TemporaryDirectory(prefix="libreecho-networkd-stale-") as temp:
        directory = Path(temp)
        process, wpa, adapter, actions, reboot = start_daemon(
            directory, "1," + ",".join("0" for _ in range(32)),
            preexisting_reboot="fastboot\n")
        try:
            wait_for(lambda: read_actions(actions)[-1:] == ["reboot-request"],
                     message="stale-request escalation")
            time.sleep(0.1)
            assert reboot.read_text() == "fastboot\n"
            status = adapter_request(adapter, 5, "status")
            assert status["data"]["recovery_stage"] == "exhausted"
        finally:
            stop_daemon(process, wpa)


def test_non_regular_reboot_request_fails_closed_without_blocking():
    with tempfile.TemporaryDirectory(prefix="libreecho-networkd-fifo-") as temp:
        directory = Path(temp)
        process, wpa, adapter, actions, reboot = start_daemon(
            directory, "1," + ",".join("0" for _ in range(32)),
            reboot_fifo=True)
        try:
            wait_for(lambda: read_actions(actions)[-1:] == ["reboot-request"],
                     message="FIFO-request escalation")
            status = adapter_request(adapter, 6, "status")
            assert status["data"]["recovery_stage"] == "exhausted"
            assert reboot.is_fifo()
        finally:
            stop_daemon(process, wpa)


def test_action_failures_remain_bounded():
    with tempfile.TemporaryDirectory(prefix="libreecho-networkd-failure-") as temp:
        directory = Path(temp)
        process, wpa, adapter, actions, reboot = start_daemon(
            directory, "1," + ",".join("0" for _ in range(32)),
            fail_reassociate=True, fail_interface="all",
            invalid_reboot_path=True)
        try:
            wait_for(lambda: read_actions(actions)[-1:] == ["reboot-request"],
                     message="failed reboot escalation")
            expected = ["reassociate", "interface-down", "interface-up",
                        "reboot-request"]
            assert read_actions(actions) == expected
            time.sleep(0.15)
            assert read_actions(actions) == expected
            assert not reboot.exists()
            status = adapter_request(adapter, 3, "status")
            assert status["data"]["recovery_stage"] == "exhausted"
        finally:
            stop_daemon(process, wpa)


def test_pending_association_keeps_daemon_responsive_and_restores_profile():
    with tempfile.TemporaryDirectory(prefix="libreecho-networkd-connect-") as temp:
        directory = Path(temp)
        process, wpa, adapter, actions, reboot = start_daemon(
            directory, "1,1,1", association_fails=True)
        try:
            client = queue_adapter_request(
                adapter, 40, "connect",
                {"ssid": "UnavailableNet", "psk": "", "security": "open"})
            try:
                started = time.monotonic()
                status = adapter_request(adapter, 41, "status")
                assert time.monotonic() - started < 0.5
                assert status["ok"] is True
                result = read_adapter_response(client, 40)
            finally:
                client.close()
            assert result["ok"] is False
            assert "association" in result["error"].lower()
            assert "REMOVE_NETWORK all" not in wpa.commands
            assert "REMOVE_NETWORK 1" in wpa.commands
            assert "SELECT_NETWORK 0" in wpa.commands
            assert wpa.network_id == 0
            assert wpa.connected is True
        finally:
            stop_daemon(process, wpa)


def test_scan_deduplicates_ssid_and_keeps_strongest_bssid():
    rows = (
        "bssid / frequency / signal level / flags / ssid\n"
        "00:11:22:33:44:55\t2412\t-79\t[WPA2-PSK-CCMP][ESS]\tMeshNet\n"
        "00:11:22:33:44:66\t2437\t-41\t[WPA2-PSK-CCMP][ESS]\tMeshNet\n"
    )
    with tempfile.TemporaryDirectory(prefix="libreecho-networkd-scan-") as temp:
        directory = Path(temp)
        process, wpa, adapter, actions, reboot = start_daemon(
            directory, "1,1,1", scan_results=rows)
        try:
            result = adapter_request(adapter, 50, "scan", timeout=4)
            assert result["ok"] is True, result
            networks = result["data"]["networks"]
            assert [entry["ssid"] for entry in networks] == ["MeshNet"]
            assert networks[0]["signal"] > 70, networks
        finally:
            stop_daemon(process, wpa)


def main():
    test_ordered_recovery_and_one_shot_reboot()
    test_recovery_can_succeed_without_supervisor_request()
    test_explicit_commands_cancel_pending_recovery()
    test_persistent_reboot_guard_blocks_cross_boot_loop()
    test_stale_non_reboot_request_is_not_accepted()
    test_non_regular_reboot_request_fails_closed_without_blocking()
    test_action_failures_remain_bounded()
    test_pending_association_keeps_daemon_responsive_and_restores_profile()
    test_scan_deduplicates_ssid_and_keeps_strongest_bssid()
    print("networkd event-loop recovery integration: ok")


if __name__ == "__main__":
    main()
