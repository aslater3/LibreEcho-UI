#!/usr/bin/env python3
"""Smoke-test the rootless virtual Echo service harness."""
from __future__ import annotations

import json
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNNER = HERE / "virtual_echo.py"


def http(port: int, path: str) -> tuple[int, bytes]:
    request = urllib.request.Request(f"http://127.0.0.1:{port}{path}")
    try:
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.status, response.read()
    except urllib.error.HTTPError as error:
        return error.code, error.read()


def adapter_call(path: Path, command: str) -> dict:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(3)
        client.connect(str(path))
        client.sendall((json.dumps({"v": 1, "id": 1, "cmd": command, "args": {}}) + "\n").encode())
        return json.loads(client.makefile().readline())


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="libreecho-virtual-") as temp:
        root = Path(temp)
        port = 18181
        process = subprocess.Popen(
            ["python3", str(RUNNER), "start", "--root", str(root), "--port", str(port)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    assert process.stderr is not None
                    raise RuntimeError(process.stderr.read())
                if (root / "run/libreecho/network.sock").exists():
                    break
                time.sleep(0.1)
            assert http(port, "/")[0] == 200
            assert http(port, "/api/v1/status")[0] == 401
            network = adapter_call(root / "run/libreecho/network.sock", "status")
            assert network["ok"] is True
            assert network["data"]["connectivity"] == "healthy"
            subprocess.run(
                ["python3", str(RUNNER), "fault", "network", "--root", str(root)],
                check=True, stdout=subprocess.PIPE, text=True,
            )
            failed = adapter_call(root / "run/libreecho/network.sock", "status")
            assert failed["ok"] is False
            print("virtual_echo_smoke=PASS")
            return 0
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    raise SystemExit(main())
