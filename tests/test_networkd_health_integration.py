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
    def __init__(self, path: Path, fail_reassociate: bool = False):
        self.path = path
        self.fail_reassociate = fail_reassociate
        self.connected = True
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
            if command == "STATUS":
                state = "COMPLETED" if self.connected else "DISCONNECTED"
                response = f"wpa_state={state}\nssid=IntegrationNet\nid=0\n"
            elif command == "SIGNAL_POLL":
                response = "RSSI=-45\n"
            elif command == "ADD_NETWORK":
                response = "0\n"
            elif command == "DISCONNECT":
                self.connected = False
                response = "OK\n"
            elif command.startswith("SELECT_NETWORK"):
                self.connected = True
                response = "OK\n"
            elif command == "REASSOCIATE" and self.fail_reassociate:
                response = "FAIL\n"
            else:
                response = "OK\n"
            try:
                self.sock.sendto(response.encode(), peer)
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
                          args=None):
    request = {"v": 1, "id": request_id, "cmd": command,
               "args": args if args is not None else {}}
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(2)
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


def adapter_request(path: Path, request_id: int, command: str, args=None):
    with queue_adapter_request(path, request_id, command, args) as client:
        return read_adapter_response(client, request_id)


def start_daemon(directory: Path, script: str, *, fail_reassociate=False,
                 fail_interface=None, invalid_reboot_path=False,
                 barrier_at=None, preexisting_reboot=None,
                 preexisting_guard=False, reboot_fifo=False):
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
    wpa = FakeWpa(wpa_path, fail_reassociate=fail_reassociate)
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


def main():
    test_ordered_recovery_and_one_shot_reboot()
    test_recovery_can_succeed_without_supervisor_request()
    test_explicit_commands_cancel_pending_recovery()
    test_persistent_reboot_guard_blocks_cross_boot_loop()
    test_stale_non_reboot_request_is_not_accepted()
    test_non_regular_reboot_request_fails_closed_without_blocking()
    test_action_failures_remain_bounded()
    print("networkd event-loop recovery integration: ok")


if __name__ == "__main__":
    main()
