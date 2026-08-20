#!/usr/bin/env python3
"""Regression test for issue #94: absent adapter GETs are clean API results."""
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
         headers: dict[str, str] | None = None) -> tuple[int, dict]:
    request_headers = {"Content-Type": "application/json", **(headers or {})}
    data = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(
        base + path, data=data, method=method, headers=request_headers
    )
    try:
        with urllib.request.urlopen(request, timeout=3) as response:
            return response.status, json.loads(response.read())
    except urllib.error.HTTPError as error:
        return error.code, json.loads(error.read())


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="libreecho-issue-94-") as temp:
        root = Path(temp)
        port = 18194
        process = subprocess.Popen(
            ["python3", str(RUNNER), "start", "--root", str(root),
             "--host", "127.0.0.1", "--port", str(port)],
            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        )
        try:
            base = f"http://127.0.0.1:{port}"
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                try:
                    status, config = call(base, "/api/v1/config")
                    if status == 200:
                        break
                except (OSError, ValueError):
                    pass
                time.sleep(0.1)
            else:
                raise AssertionError("virtual target did not become ready")

            csrf = config["data"]["csrf_token"]
            status, session = call(
                base, "/api/v1/auth/bootstrap", "POST",
                {"username": "admin", "password": "test-password-123",
                 "password_confirm": "test-password-123"},
                {"X-LibreEcho-CSRF": csrf},
            )
            assert status == 200, session
            auth = {"Authorization": f"Bearer {session['data']['token']}"}

            # A live service fault must remain an API error, not look absent.
            (root / "state" / "virtual.json").write_text(
                json.dumps({"faults": ["audio"]}) + "\n"
            )
            status, response = call(base, "/api/v1/audio", headers=auth)
            assert status == 503, (status, response)
            assert response["ok"] is False, response
            assert response["error"]["code"] == "io_error", response

            # Remove the disposable socket names to model absent hardware daemons.
            for service in ("led", "audio", "wakeword", "bluetooth"):
                (root / "run/libreecho" / f"{service}.sock").unlink()

            for path in ("/api/v1/led", "/api/v1/audio", "/api/v1/wake-word",
                         "/api/v1/bluetooth"):
                status, response = call(base, path, headers=auth)
                assert status == 200, (path, status, response)
                assert response["ok"] is True, (path, response)
                assert response["data"]["available"] is False, (path, response)
                assert response["data"]["unavailable"] is True, (path, response)

            status, response = call(
                base, "/api/v1/audio", "PUT", {"volume": 25},
                {**auth, "X-LibreEcho-CSRF": csrf},
            )
            assert status == 501, (status, response)
            print("issue_94_absent_adapter_gets=PASS")
            return 0
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    raise SystemExit(main())
