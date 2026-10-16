#!/usr/bin/env python3
"""Prove the production root-peer gate on the secret recovery commands.

networkd gates `recovery_prepare|psk|configure|stop` on a root peer
(SO_PEERCRED uid 0) so only the control plane can reveal or re-key the
provisioning password. The host lifecycle fixture is built with
LE_NETWORKD_TESTING, which *exempts* that gate -- so it can never show whether
the shipped gate works.

This test builds networkd WITHOUT LE_NETWORKD_TESTING (the production code
path) and connects from an unprivileged uid. All four recovery commands must be
refused with "owner authorization required". A non-secret command (`status`)
must still succeed, so the refusal is the gate and not a dead daemon.

Environment (set by tests/run_recovery_backend_integration.sh):
    LE_F3_NETWORKD_PROD   networkd built without LE_NETWORKD_TESTING
"""

import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
PROD = Path(os.environ.get(
    "LE_F3_NETWORKD_PROD", str(ROOT / "build" / "libreecho-networkd-prod")))
UNPRIVILEGED_UID = 65534


def send_request(sock_path, request_id, command, args=None, timeout=3):
    payload = json.dumps({"v": 1, "id": request_id, "cmd": command,
                          "args": args if args is not None else {}}) + "\n"
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(timeout)
    client.connect(str(sock_path))
    client.sendall(payload.encode())
    with client:
        reader = client.makefile("r", encoding="utf-8")
        line = reader.readline()
        if not line:
            raise AssertionError(f"connection closed for {command}")
        return json.loads(line)


def send_request_unprivileged(sock_path, request_id, command, args=None,
                             timeout=5):
    """Run send_request from a non-root uid.

    When this test itself runs as root (some CI), the client must drop
    privileges so SO_PEERCRED reports an unprivileged peer; the daemon's socket
    is widened to 0666 for the duration so the connection reaches the gate
    instead of failing on file permissions.
    """
    if os.geteuid() != 0:
        return send_request(sock_path, request_id, command, args, timeout)
    read_fd, write_fd = os.pipe()
    pid = os.fork()
    if pid == 0:  # child: unprivileged client
        try:
            os.close(read_fd)
            try:
                os.setgroups([])
            except OSError:
                pass
            os.setgid(UNPRIVILEGED_UID)
            os.setuid(UNPRIVILEGED_UID)
            try:
                response = send_request(sock_path, request_id, command, args,
                                        timeout)
                os.write(write_fd, json.dumps(response).encode())
            except Exception as error:  # noqa: BLE001 - report to parent
                os.write(write_fd, json.dumps(
                    {"client_error": f"{type(error).__name__}: {error}"}).encode())
        finally:
            os._exit(0)
    os.close(write_fd)
    chunks = []
    with os.fdopen(read_fd, "rb") as stream:
        chunks.append(stream.read())
    os.waitpid(pid, 0)
    return json.loads(b"".join(chunks).decode())


def wait_for_socket(path, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        time.sleep(0.02)
    raise AssertionError(f"networkd socket {path} did not appear")


def test_production_peer_gate_refuses_all_recovery_commands():
    with tempfile.TemporaryDirectory(prefix="le-peer-gate-") as temp:
        directory = Path(temp)
        os.chmod(directory, 0o700)
        (directory / "run").mkdir()
        sock = directory / "network.sock"
        args = [
            str(PROD), "--foreground", "--quiet",
            "--socket", str(sock),
            "--wpa-ctrl", str(directory / "wpa.sock"),
            "--interface", "test0",
            "--reboot-request", str(directory / "reboot.request"),
            "--reboot-guard", str(directory / "reboot.guard"),
            "--recovery-marker", str(directory / "recovery-mode"),
            "--recovery-psk", str(directory / "recovery-psk"),
            "--recovery-config", str(directory / "recovery.json"),
            "--recovery-run-dir", str(directory / "run"),
            "--led-socket", str(directory / "led.sock"),
        ]
        log = open(directory / "networkd.log", "wb")
        process = subprocess.Popen(args, cwd=ROOT, stdout=log, stderr=log,
                                   stdin=subprocess.DEVNULL)
        try:
            wait_for_socket(sock)
            if os.geteuid() == 0:
                # See send_request_unprivileged: let a dropped-uid peer reach
                # the gate rather than the mode-0660 file check.
                os.chmod(sock, 0o666)
            commands = [
                ("recovery_prepare", None),
                ("recovery_psk", None),
                ("recovery_configure", {"enabled": True, "auto_enabled": False,
                                        "auto_timeout_ms": 60000}),
                ("recovery_stop", None),
            ]
            for index, (command, args) in enumerate(commands):
                response = send_request_unprivileged(sock, index + 1, command,
                                                     args)
                assert response.get("ok") is False, (command, response)
                assert response.get("error") == "owner authorization required", \
                    (command, response)
            # The gate is specific: an ordinary command still answers normally.
            control = send_request_unprivileged(sock, 99, "status")
            assert control.get("ok") is True, control
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
            log.close()


def main():
    tests = [test_production_peer_gate_refuses_all_recovery_commands]
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
        print(f"recovery-peer-gate-failures={failed}")
        return 1
    print(f"recovery-peer-gate-ok={len(tests)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
