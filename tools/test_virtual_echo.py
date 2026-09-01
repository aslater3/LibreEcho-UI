#!/usr/bin/env python3
"""Smoke-test the rootless virtual Echo service harness."""
from __future__ import annotations

import json

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
    except urllib.error.URLError:
        return 0, b""


def test_persisted_auth_root() -> None:
    with tempfile.TemporaryDirectory(prefix="libreecho-virtual-auth-") as temp:
        root = Path(temp)
        port = 18182
        users = root / "data/libreecho/config/users"
        users.parent.mkdir(parents=True)
        record = subprocess.check_output(
            ["sh", str(HERE / "create-user.sh"), "admin", "test-password-123"],
            text=True,
        )
        users.write_text(record)
        users.chmod(0o600)
        process = subprocess.Popen(
            ["python3", str(RUNNER), "start", "--root", str(root), "--port", str(port)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            deadline = time.monotonic() + 10
            ready = False
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    assert process.stderr is not None
                    raise RuntimeError(process.stderr.read())
                if (root / "run/libreecho/network.sock").exists() and http(port, "/")[0] == 200:
                    ready = True
                    break
                time.sleep(0.1)
            assert ready
            assert (root / "data/libreecho/config/web-config.json").is_file()
            status, page = http(port, "/")
            assert status == 200
            assert b"LibreEcho Control Centre" in page
            status, body = http(port, "/api/v1/config")
            assert status == 200
            config = json.loads(body)["data"]
            assert config["authentication"] == "users"
            assert config["bootstrap_required"] is False
            assert http(port, "/api/v1/status")[0] == 401
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()


def main() -> int:
    test_persisted_auth_root()
    with tempfile.TemporaryDirectory(prefix="libreecho-virtual-") as temp:
        root = Path(temp)
        port = 18181
        process = subprocess.Popen(
            ["python3", str(RUNNER), "start", "--root", str(root), "--port", str(port)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            deadline = time.monotonic() + 10
            ready = False
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    assert process.stderr is not None
                    raise RuntimeError(process.stderr.read())
                if (root / "run/libreecho/network.sock").exists() and http(port, "/")[0] == 200:
                    ready = True
                    break
                time.sleep(0.1)
            assert ready
            assert http(port, "/")[0] == 200
            css_status, css_body = http(port, "/css/app.css?rev=test")
            assert css_status == 200
            assert css_body.startswith(b"/*") or b"--" in css_body
            assert http(port, "/api/v1/status")[0] == 401
            status, _ = http(port, "/api/v1/network")
            assert status == 401
            _, config = http(port, "/api/v1/config")
            csrf = json.loads(config)["data"]["csrf_token"]
            session_request = urllib.request.Request(
                f"http://127.0.0.1:{port}/api/v1/auth/bootstrap",
                data=json.dumps({"username": "admin", "password": "test-password-123",
                                 "password_confirm": "test-password-123"}).encode(),
                method="POST", headers={"Content-Type": "application/json",
                                         "X-LibreEcho-CSRF": csrf})
            with urllib.request.urlopen(session_request, timeout=3) as response:
                token = json.loads(response.read())["data"]["token"]
            network_request = urllib.request.Request(
                f"http://127.0.0.1:{port}/api/v1/network",
                headers={"Authorization": f"Bearer {token}"})
            with urllib.request.urlopen(network_request, timeout=3) as response:
                network = json.loads(response.read())
            assert network["ok"] is True
            assert network["data"]["connectivity"] == "healthy"
            subprocess.run(
                ["python3", str(RUNNER), "fault", "network", "--root", str(root)],
                check=True, stdout=subprocess.PIPE, text=True,
            )
            network_request = urllib.request.Request(
                f"http://127.0.0.1:{port}/api/v1/network",
                headers={"Authorization": f"Bearer {token}"})
            try:
                with urllib.request.urlopen(network_request, timeout=3) as response:
                    failed = json.loads(response.read())
            except urllib.error.HTTPError as error:
                raise AssertionError(f"network adapter fault returned HTTP {error.code}") from error
            assert failed["ok"] is True
            assert failed["data"]["connectivity"] == "disconnected"
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
