#!/usr/bin/env python3
"""Regression test for issue #34 using the virtual Echo target."""
from __future__ import annotations

import json
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "tools" / "virtual_echo.py"


def call(base: str, path: str, method: str = "GET", body: dict | None = None,
         token: str = "", csrf: str = "") -> tuple[int, dict]:
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if csrf:
        headers["X-LibreEcho-CSRF"] = csrf
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(base + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="libreecho-issue-34-") as temp:
        root = Path(temp)
        port = 18183
        process = subprocess.Popen(
            ["python3", str(RUNNER), "start", "--root", str(root),
             "--host", "127.0.0.1", "--port", str(port)],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if (root / "run/libreecho/network.sock").exists():
                    break
                time.sleep(0.1)
            base = f"http://127.0.0.1:{port}"
            _, config = call(base, "/api/v1/config")
            csrf = config["data"]["csrf_token"]
            _, session = call(base, "/api/v1/auth/bootstrap", "POST", {
                "username": "admin", "password": "test-password-123",
                "password_confirm": "test-password-123"}, csrf=csrf)
            token = session["data"]["token"]
            _, network = call(base, "/api/v1/network", token=token)
            _, integrations = call(base, "/api/v1/integrations", token=token)
            rest = next(item for item in integrations["data"]["items"] if item["id"] == "rest")
            assert rest["enabled"] == network["data"]["api_lan_effective"]
            assert rest["forced"] == network["data"]["api_lan_forced"]

            call(base, "/api/v1/integrations/rest", "PUT", {"enabled": False}, token, csrf)
            _, network_after_rest = call(base, "/api/v1/network", token=token)
            _, integrations_after_rest = call(base, "/api/v1/integrations", token=token)
            rest_after = next(item for item in integrations_after_rest["data"]["items"] if item["id"] == "rest")
            assert network_after_rest["data"]["api_lan"] is False
            assert rest_after["enabled"] == network_after_rest["data"]["api_lan_effective"]

            call(base, "/api/v1/network", "PUT", {"api_lan": True}, token, csrf)
            _, network_after_network = call(base, "/api/v1/network", token=token)
            _, integrations_after_network = call(base, "/api/v1/integrations", token=token)
            rest_after_network = next(item for item in integrations_after_network["data"]["items"] if item["id"] == "rest")
            assert network_after_network["data"]["api_lan"] is True
            assert rest_after_network["enabled"] == network_after_network["data"]["api_lan_effective"]
            print("issue_34_toggle_consistency=PASS")
            return 0
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()


if __name__ == "__main__":
    raise SystemExit(main())
