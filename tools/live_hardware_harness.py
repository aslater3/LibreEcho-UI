#!/usr/bin/env python3
"""Live LibreEcho hardware validation harness.

Safe by default:
  - discovers the live API from /openapi.json;
  - authenticates only with LIBREECHO_USERNAME/LIBREECHO_PASSWORD;
  - runs all safe GET/SSE/static checks;
  - gathers read-only ADB-root evidence and AirPlay RTSP evidence;
  - never performs a reboot, reset, OTA install, radio mutation, audio test,
    LED test, Wi-Fi disconnect, or configuration write unless an explicit
    exercise flag is supplied.

Examples:
  python3 tools/live_hardware_harness.py \
    --url http://192.0.2.10:8080 --adb-serial DEVICE_SERIAL
  LIBREECHO_LIVE_URL=http://192.0.2.10:8080 ADB_SERIAL=DEVICE_SERIAL \
    LIBREECHO_USERNAME=... LIBREECHO_PASSWORD=... \
    python3 tools/live_hardware_harness.py --strict-capabilities
  python3 tools/live_hardware_harness.py \
    --url http://192.0.2.10:8080 --adb-serial DEVICE_SERIAL \
    --capture --report build/live.json

`--mutate` performs same-state PUT/readback checks for reversible configuration
routes. `--exercise-hardware` runs bounded audio, LED, wake-word, Bluetooth
scan, diagnostics, and update-check actions; destructive power routes are only
safety-probed without their confirmation header. `--capture` opens the live
microphone stream endpoint and is not enabled by default.

Passwords and bearer/CSRF tokens are never emitted in reports.
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from typing import Any, Dict, Iterable, List, Optional, Tuple

SECRET_KEYS = re.compile(r"(?:token|password|secret|api[_-]?key|authorization|credential)", re.I)
DESTRUCTIVE_PATHS = {
    "/api/v1/system/reboot",
    "/api/v1/system/shutdown",
    "/api/v1/system/factory-reset",
}
CAPTURE_PATHS = {"/api/v1/baby-monitor/stream"}
CAPTURE_QUERY = "?source=0%3A24&channel=0"
STATIC_PATHS = ["/", "/swagger.html", "/assets/favicon.svg", "/assets/mark.svg"]


@dataclass
class Result:
    name: str
    status: str
    detail: str = ""
    http_status: Optional[int] = None
    data: Any = None
    duration_ms: Optional[int] = None

    def as_dict(self) -> Dict[str, Any]:
        out: Dict[str, Any] = {
            "name": self.name,
            "status": self.status,
            "detail": self.detail,
        }
        if self.http_status is not None:
            out["http_status"] = self.http_status
        if self.duration_ms is not None:
            out["duration_ms"] = self.duration_ms
        if self.data is not None:
            out["data"] = redact_secrets(self.data)
        return out


@dataclass
class HttpResponse:
    status: int
    headers: Dict[str, str]
    body: bytes
    json: Any = None
    error: str = ""


@dataclass
class Harness:
    base_url: str
    timeout: float = 8.0
    adb_serial: str = ""
    strict_capabilities: bool = False
    capture: bool = False
    mutate: bool = False
    exercise_hardware: bool = False
    results: List[Result] = field(default_factory=list)
    csrf: str = ""
    bearer: str = ""
    openapi: Dict[str, Any] = field(default_factory=dict)
    config: Dict[str, Any] = field(default_factory=dict)

    def __post_init__(self) -> None:
        self.base_url = self.base_url.rstrip("/")
        self.client = HttpClient(self.base_url, self.timeout)

    def add(self, result: Result) -> None:
        self.results.append(result)
        print(f"[{result.status.upper():11}] {result.name}: {result.detail}")

    def request(self, method: str, path: str, body: Any = None,
                authenticated: bool = True, timeout: Optional[float] = None,
                extra_headers: Optional[Dict[str, str]] = None) -> HttpResponse:
        headers = {"Accept": "application/json, text/event-stream, */*"}
        if authenticated and self.bearer:
            headers["Authorization"] = f"Bearer {self.bearer}"
        if self.csrf and method not in {"GET", "HEAD"}:
            headers["X-LibreEcho-CSRF"] = self.csrf
        if extra_headers:
            headers.update(extra_headers)
        return self.client.request(method, path, body, headers, timeout)

    def run(self) -> int:
        self.discovery_checks()
        self.authentication_checks()
        self.api_route_checks()
        self.static_checks()
        self.airplay_check()
        self.adb_checks()
        if self.mutate:
            self.same_state_round_trips()
        if self.exercise_hardware:
            self.transient_hardware_checks()
        return self.finish()

    def discovery_checks(self) -> None:
        started = time.monotonic()
        response = self.client.request("GET", "/openapi.json")
        if response.status != 200 or not isinstance(response.json, dict):
            self.add(Result("openapi", "fail", f"HTTP {response.status}: {response.error}"))
            return
        self.openapi = response.json
        paths = self.openapi.get("paths")
        if not isinstance(paths, dict) or not paths:
            self.add(Result("openapi", "fail", "missing non-empty paths object"))
            return
        self.add(Result("openapi", "pass", f"{len(paths)} declared paths", 200,
                        {"path_count": len(paths)}, elapsed_ms(started)))

        config = self.client.request("GET", "/api/v1/config")
        if config.status != 200 or not isinstance(config.json, dict):
            self.add(Result("api-config", "fail", f"HTTP {config.status}: {config.error}"))
            return
        self.config = config.json
        error = validate_envelope(config.json)
        if error:
            self.add(Result("api-config", "fail", error, config.status))
            return
        data = config.json.get("data") or {}
        self.csrf = str(data.get("csrf_token") or "")
        auth_mode = str(data.get("authentication") or "")
        if not re.fullmatch(r"[0-9a-fA-F]{64}", self.csrf):
            self.add(Result("api-config-csrf", "fail", "CSRF token is not a 64-character hex value", config.status))
        else:
            self.add(Result("api-config-csrf", "pass", f"authentication={auth_mode}"))

        root = self.request("GET", "/api/v1/", authenticated=False)
        if root.status == 200 and isinstance(root.json, dict) and not validate_envelope(root.json):
            self.add(Result("api-discovery", "pass", "base API discovery responds", root.status))
        elif root.status in {401, 403} and auth_mode not in {"", "development-disabled"}:
            self.add(Result("api-discovery-anonymous", "pass", "base API discovery is protected before login", root.status))
        else:
            self.add(Result("api-discovery", "fail", f"HTTP {root.status}: {root.error}", root.status))

    def authentication_checks(self) -> None:
        auth_mode = str((self.config.get("data") or {}).get("authentication") or "")
        protected = self.client.request("GET", "/api/v1/status")
        if auth_mode in {"users", "bearer-token"}:
            if protected.status in {401, 403}:
                self.add(Result("anonymous-protection", "pass", f"protected endpoint returns HTTP {protected.status}", protected.status))
            else:
                self.add(Result("anonymous-protection", "fail", f"expected 401/403, got {protected.status}", protected.status))
        else:
            self.add(Result("anonymous-protection", "pass", f"authentication mode={auth_mode or 'disabled'}"))

        username = os.environ.get("LIBREECHO_USERNAME", "")
        password = os.environ.get("LIBREECHO_PASSWORD", "")
        if auth_mode == "users":
            if not username or not password:
                self.add(Result("authenticated-session", "blocked", "set LIBREECHO_USERNAME and LIBREECHO_PASSWORD; credentials are never printed"))
                return
            response = self.client.request(
                "POST", "/api/v1/auth/login",
                {"username": username, "password": password},
                headers={"X-LibreEcho-CSRF": self.csrf},
            )
            token = ((response.json or {}).get("data") or {}).get("token") if isinstance(response.json, dict) else None
            if response.status != 200 or not token:
                self.add(Result("authenticated-session", "fail", f"login failed with HTTP {response.status}", response.status))
                return
            self.bearer = str(token)
            current = self.request("GET", "/api/v1/auth")
            if current.status == 200 and not validate_envelope(current.json):
                self.add(Result("authenticated-session", "pass", "local user session established", current.status))
                discovery = self.request("GET", "/api/v1/")
                if discovery.status == 200 and isinstance(discovery.json, dict) and not validate_envelope(discovery.json):
                    self.add(Result("api-discovery", "pass", "base API discovery responds after authentication", discovery.status))
                else:
                    self.add(Result("api-discovery", "fail", f"authenticated discovery HTTP {discovery.status}: {discovery.error}", discovery.status))
            else:
                self.add(Result("authenticated-session", "fail", f"session validation HTTP {current.status}", current.status))
        elif auth_mode == "bearer-token":
            token = os.environ.get("LIBREECHO_BEARER_TOKEN", "")
            if not token:
                self.add(Result("authenticated-session", "blocked", "set LIBREECHO_BEARER_TOKEN; it is never printed"))
            else:
                self.bearer = token
                response = self.request("GET", "/api/v1/status")
                authenticated = response.status == 200
                self.add(Result("authenticated-session", "pass" if authenticated else "fail", f"bearer validation HTTP {response.status}", response.status))
                if authenticated:
                    discovery = self.request("GET", "/api/v1/")
                    if discovery.status == 200 and isinstance(discovery.json, dict) and not validate_envelope(discovery.json):
                        self.add(Result("api-discovery", "pass", "base API discovery responds after authentication", discovery.status))
                    else:
                        self.add(Result("api-discovery", "fail", f"authenticated discovery HTTP {discovery.status}: {discovery.error}", discovery.status))
        else:
            self.add(Result("authenticated-session", "pass", "no authentication required"))

    def api_route_checks(self) -> None:
        paths = self.openapi.get("paths", {})
        read_routes: List[str] = []
        for path, methods in paths.items():
            if not isinstance(methods, dict):
                continue
            actual = "/api/v1" + path
            for method in sorted(methods):
                if method.lower() not in {"get", "put", "post", "delete", "patch"}:
                    continue
                operation = classify_operation(method, actual)
                if method.lower() != "get":
                    if operation == "destructive":
                        detail = "never invoked by harness; requires separate operator confirmation"
                    elif operation == "capture":
                        detail = "requires --capture and explicit capture acceptance"
                    else:
                        detail = "not invoked in safe mode; use --mutate/--exercise-hardware"
                    self.add(Result(f"GATE {method.upper()} {actual}", "gated", detail))
            if "get" not in methods:
                continue
            if actual in CAPTURE_PATHS and not self.capture:
                self.add(Result(f"GET {actual}", "skip", "microphone stream requires --capture"))
                continue
            if actual in CAPTURE_PATHS:
                actual += CAPTURE_QUERY
            read_routes.append(actual)
        # The OpenAPI document describes /api/v1 as path '/', but the deployed
        # handler accepts both /api/v1 and /api/v1/.
        if "/api/v1/" not in read_routes:
            read_routes.append("/api/v1/")
        for path in sorted(set(read_routes)):
            self.check_read_route(path)

    def check_read_route(self, path: str) -> None:
        started = time.monotonic()
        route_path = path.split("?", 1)[0]
        response = self.request("GET", path, timeout=12.0 if route_path.endswith("/stream") else None)
        elapsed = elapsed_ms(started)
        content_type = response.headers.get("content-type", "")
        if response.status in {401, 403} and not self.bearer:
            self.add(Result(f"GET {path}", "blocked", "authentication required; provide credentials", response.status, duration_ms=elapsed))
            return
        route_path = path.split("?", 1)[0]
        if route_path in CAPTURE_PATHS:
            if response.status == 200 and ("audio" in content_type or "octet-stream" in content_type):
                status = "pass"
            elif response.status in {501, 503}:
                status = "unsupported"
            else:
                status = "fail"
            self.add(Result(f"GET {path}", status, f"HTTP {response.status} content-type={content_type or 'none'}", response.status, duration_ms=elapsed))
            return
        if response.status in {501, 503}:
            self.add(Result(f"GET {path}", "unsupported", f"HTTP {response.status}: {error_message(response.json)}", response.status, duration_ms=elapsed))
            return
        if response.status != 200:
            self.add(Result(f"GET {path}", "fail", f"HTTP {response.status}: {error_message(response.json) or response.error}", response.status, duration_ms=elapsed))
            return
        if "text/event-stream" in content_type:
            if b"event:" in response.body:
                self.add(Result(f"GET {path}", "pass", "SSE snapshot contains event records", response.status, duration_ms=elapsed))
            else:
                self.add(Result(f"GET {path}", "fail", "SSE response has no event records", response.status, duration_ms=elapsed))
            return
        if not isinstance(response.json, dict):
            self.add(Result(f"GET {path}", "fail", f"HTTP 200 is not JSON ({content_type or 'no content type'})", response.status, duration_ms=elapsed))
            return
        envelope_error = validate_envelope(response.json)
        if envelope_error:
            self.add(Result(f"GET {path}", "fail", envelope_error, response.status, duration_ms=elapsed))
            return
        if response.json.get("ok") is False:
            self.add(Result(f"GET {path}", "unsupported", error_message(response.json), response.status, redact_secrets(response.json.get("error")), elapsed))
            return
        semantic_error = validate_semantics(path, response.json.get("data"))
        if semantic_error:
            self.add(Result(f"GET {path}", "fail", semantic_error, response.status, duration_ms=elapsed))
        else:
            self.add(Result(f"GET {path}", "pass", "valid response envelope", response.status, duration_ms=elapsed))

    def static_checks(self) -> None:
        for path in STATIC_PATHS:
            response = self.client.request("GET", path)
            if response.status == 200 and response.body:
                self.add(Result(f"static {path}", "pass", f"{len(response.body)} bytes", response.status))
            else:
                self.add(Result(f"static {path}", "fail", f"HTTP {response.status}: {response.error}", response.status))

    def airplay_check(self) -> None:
        host, port = urllib.parse.urlsplit(self.base_url).hostname, 7000
        if not host:
            self.add(Result("airplay-rtsp", "skip", "API URL has no host"))
            return
        started = time.monotonic()
        request = b"OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n"
        try:
            with socket.create_connection((host, port), timeout=self.timeout) as sock:
                sock.settimeout(self.timeout)
                sock.sendall(request)
                data = sock.recv(4096)
            if b"RTSP/1.0 200" in data and b"Public:" in data:
                self.add(Result("airplay-rtsp", "pass", "OPTIONS returned RTSP 200 with Public methods", duration_ms=elapsed_ms(started)))
            else:
                self.add(Result("airplay-rtsp", "fail", data[:160].decode("ascii", "replace"), duration_ms=elapsed_ms(started)))
        except OSError as exc:
            self.add(Result("airplay-rtsp", "fail", str(exc), duration_ms=elapsed_ms(started)))

    def adb_checks(self) -> None:
        script = r'''#!/bin/busybox sh
set +e
printf '%s\n' AUDIT_START
id
uname -a
awk '$1 ~ /^(MemTotal|MemAvailable|SwapTotal|SwapFree):/ {print}' /proc/meminfo
ip addr show 2>&1
ip route 2>&1
for p in /dev/wmt /dev/wmtWifi /dev/stpbt /dev/snd /sys/class/net/wlan0 /sys/class/bluetooth/hci0 /run/libreecho/audio.sock /run/libreecho/mic.sock /run/libreecho/bluetooth.sock /run/libreecho/agent.sock /run/libreecho/tts.sock /run/libreecho/stt.sock; do
  if [ -e "$p" ]; then printf 'present %s\n' "$p"; else printf 'absent %s\n' "$p"; fi
done
printf '%s\n' '=== processes ==='
ps 2>/dev/null | awk 'NR==1 || $0 ~ /libreecho|adbd|wpa_supplicant|shairport|mtk_wmtd|btif/'
printf '%s\n' '=== audio ==='
sed -n '1,80p' /proc/asound/cards 2>&1
sed -n '1,120p' /proc/asound/pcm 2>&1
printf '%s\n' '=== bluetooth ==='
for p in /sys/class/bluetooth/hci0/rfkill0/soft /sys/class/bluetooth/hci0/rfkill0/state /sys/class/bluetooth/hci0/rfkill0/hard; do [ -r "$p" ] && printf '%s=' "$p" && sed -n '1p' "$p"; done
printf '%s\n' '=== logs ==='
for p in /run/libreecho/time.status /tmp/wifi.status /tmp/wifi.log /tmp/data-cleanup.log; do if [ -r "$p" ]; then printf '%s\n' "--- $p ---"; sed -n '1,40p' "$p"; fi; done
dmesg 2>/dev/null | awk 'tolower($0) ~ /panic|oops|bug:|watchdog|wdt/ {print}' | tail -40
printf '%s\n' AUDIT_END
exit 0
'''
        with tempfile.NamedTemporaryFile("w", prefix="libreecho-root-audit-", suffix=".sh", delete=False) as handle:
            handle.write(script)
            script_path = handle.name
        try:
            helper = os.environ.get(
                "LIBREECHO_ROOT_HELPER",
                os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "LibreEcho-Kernel", "tools", "mt8163-arm32", "adb-run-root.sh")),
            )
            command = [helper, script_path, "45"]
            env = os.environ.copy()
            env["ADB_SERIAL"] = self.adb_serial
            started = time.monotonic()
            completed = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60, env=env)
            evidence = parse_root_evidence(completed.stdout)
            if completed.returncode != 0:
                self.add(Result("adb-root-evidence", "fail", f"helper exit {completed.returncode}", data=evidence, duration_ms=elapsed_ms(started)))
                return
            missing = [key for key in ("root", "kernel", "wlan0", "wmt", "audio_socket", "mic_socket", "bluetooth_socket") if not evidence.get(key)]
            status = "fail" if missing else "pass"
            detail = "required control-plane markers present" if not missing else "missing: " + ", ".join(missing)
            self.add(Result("adb-root-evidence", status, detail, data=evidence, duration_ms=elapsed_ms(started)))
        except (OSError, subprocess.SubprocessError) as exc:
            self.add(Result("adb-root-evidence", "fail", str(exc)))
        finally:
            try:
                os.unlink(script_path)
            except OSError:
                pass

    def same_state_round_trips(self) -> None:
        """Exercise reversible state APIs using their currently reported values."""
        candidates = {
            "/api/v1/audio": lambda data: {
                key: data[key] for key in
                ("volume", "microphone_gain", "microphone_muted", "tts_voice")
                if isinstance(data, dict) and key in data
            },
            "/api/v1/led": lambda data: led_payload(data),
            "/api/v1/network": lambda data: {
                "hostname": data.get("hostname")
            } if isinstance(data, dict) and data.get("hostname") else {},
            "/api/v1/wake-word": lambda data: {
                key: data[key] for key in ("wake_word", "sensitivity")
                if isinstance(data, dict) and key in data
            },
            "/api/v1/buttons": lambda data: {
                key: data[key] for key in ("short_press", "long_press")
                if isinstance(data, dict) and key in data
            },
            "/api/v1/privacy": lambda data: {
                key: data[key] for key in
                ("local_only", "audio_retention", "diagnostic_telemetry",
                 "log_retention_hours", "crash_reports")
                if isinstance(data, dict) and key in data
            },
            "/api/v1/voice-pipeline": lambda data: voice_pipeline_payload(data),
        }
        for path, builder in candidates.items():
            self.round_trip(path, builder)

    def round_trip(self, path: str, builder: Any) -> None:
        before = self.request("GET", path)
        if before.status in {501, 503}:
            self.add(Result(f"round-trip {path}", "unsupported", f"GET HTTP {before.status}: {error_message(before.json)}", before.status))
            return
        if before.status != 200 or not isinstance(before.json, dict) or before.json.get("ok") is not True:
            self.add(Result(f"round-trip {path}", "fail", f"snapshot HTTP {before.status}: {error_message(before.json)}", before.status))
            return
        payload = builder(before.json.get("data"))
        if not payload:
            self.add(Result(f"round-trip {path}", "skip", "current state has no supported writable fields"))
            return
        changed = self.request("PUT", path, payload)
        if changed.status in {501, 503}:
            self.add(Result(f"round-trip {path}", "unsupported", f"PUT HTTP {changed.status}: {error_message(changed.json)}", changed.status))
            return
        if changed.status != 200 or not isinstance(changed.json, dict) or changed.json.get("ok") is not True:
            self.add(Result(f"round-trip {path}", "fail", f"PUT HTTP {changed.status}: {error_message(changed.json)}", changed.status))
            return
        after = self.request("GET", path)
        if after.status != 200 or not isinstance(after.json, dict) or after.json.get("ok") is not True:
            self.add(Result(f"round-trip {path}", "fail", f"readback HTTP {after.status}: {error_message(after.json)}", after.status))
            return
        current = after.json.get("data")
        mismatches = [key for key, value in payload.items()
                      if readback_value(current, key) != value]
        if mismatches:
            self.add(Result(f"round-trip {path}", "fail", "readback mismatch: " + ", ".join(mismatches), after.status))
        else:
            self.add(Result(f"round-trip {path}", "pass", "same-state write accepted and read back", after.status))

    def transient_hardware_checks(self) -> None:
        """Run bounded, non-destructive hardware actions after explicit opt-in."""
        self.post_hardware("/api/v1/audio/test", None)
        announce = os.environ.get("LIBREECHO_TEST_ANNOUNCEMENT", "LibreEcho hardware test")
        announced = self.post_hardware("/api/v1/audio/announce", {"text": announce})
        if announced:
            self.post_hardware("/api/v1/audio/announce/stop", None)
        self.post_hardware("/api/v1/led/test", None)
        self.post_hardware("/api/v1/wake-word/test", None)
        scan_started = self.post_hardware("/api/v1/bluetooth/scan", None)
        if scan_started:
            self.post_hardware("/api/v1/bluetooth/scan/stop", None)
        self.post_hardware("/api/v1/diagnostics/export", None)
        self.post_hardware("/api/v1/system/update/check", None)
        assistant_text = os.environ.get("LIBREECHO_TEST_ASSISTANT_TEXT", "")
        if assistant_text:
            self.post_hardware("/api/v1/assistant/respond", {"text": assistant_text})
        else:
            self.add(Result("POST /api/v1/assistant/respond", "skip", "set LIBREECHO_TEST_ASSISTANT_TEXT for an explicit voice-assistant test"))
        self.safety_checks()

    def safety_checks(self) -> None:
        # Safety checks prove that destructive routes reject missing confirmation
        # without ever sending the confirmation token or invoking the action.
        for index, path in enumerate(sorted(DESTRUCTIVE_PATHS)):
            if index:
                time.sleep(3.1)
            response = self.request("POST", path)
            if response.status == 429:
                time.sleep(3.1)
                response = self.request("POST", path)
            status = "pass" if response.status == 403 else "fail"
            detail = f"missing-confirmation response HTTP {response.status}"
            if response.status == 403 and index:
                detail += "; probes spaced to avoid the device rate limiter"
            self.add(Result(f"safety {path}", status, detail, response.status))

    def post_hardware(self, path: str, body: Any) -> bool:
        response = self.request("POST", path, body)
        if response.status in {501, 503}:
            self.add(Result(f"exercise {path}", "unsupported", f"HTTP {response.status}: {error_message(response.json)}", response.status))
            return False
        if response.status in {401, 403}:
            self.add(Result(f"exercise {path}", "blocked", f"authentication/CSRF HTTP {response.status}", response.status))
            return False
        ok = response.status in {200, 202} and isinstance(response.json, dict) and response.json.get("ok") is True
        self.add(Result(f"exercise {path}", "pass" if ok else "fail", f"HTTP {response.status}: {error_message(response.json)}", response.status))
        return ok

    def finish(self) -> int:
        counts: Dict[str, int] = {}
        for result in self.results:
            counts[result.status] = counts.get(result.status, 0) + 1
        print("\nSUMMARY " + " ".join(f"{key}={counts[key]}" for key in sorted(counts)))
        failures = counts.get("fail", 0)
        if self.strict_capabilities:
            failures += counts.get("unsupported", 0) + counts.get("blocked", 0)
        return 1 if failures else 0


class HttpClient:
    def __init__(self, base_url: str, timeout: float) -> None:
        self.base_url = base_url.rstrip("/")
        self.timeout = timeout

    def request(self, method: str, path: str, body: Any = None,
                headers: Optional[Dict[str, str]] = None,
                timeout: Optional[float] = None) -> HttpResponse:
        url = self.base_url + (path if path.startswith("/") else "/" + path)
        payload = None
        request_headers = dict(headers or {})
        if body is not None:
            payload = json.dumps(body, separators=(",", ":")).encode()
            request_headers["Content-Type"] = "application/json"
        request = urllib.request.Request(url, data=payload, headers=request_headers, method=method)
        try:
            with urllib.request.urlopen(request, timeout=timeout or self.timeout) as response:
                raw = response.read(4 * 1024 * 1024)
                return parse_http_response(response.status, dict(response.headers.items()), raw)
        except urllib.error.HTTPError as exc:
            raw = exc.read(4 * 1024 * 1024)
            return parse_http_response(exc.code, dict(exc.headers.items()) if exc.headers else {}, raw, str(exc))
        except (OSError, urllib.error.URLError, TimeoutError) as exc:
            return HttpResponse(0, {}, b"", error=str(exc))


def parse_http_response(status: int, headers: Dict[str, str], body: bytes, error: str = "") -> HttpResponse:
    content_type = next((value for key, value in headers.items() if key.lower() == "content-type"), "")
    parsed = None
    if "json" in content_type.lower() or body.lstrip().startswith((b"{", b"[")):
        try:
            parsed = json.loads(body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            parsed = None
    return HttpResponse(status, {str(k).lower(): str(v) for k, v in headers.items()}, body, parsed, error)


def classify_operation(method: str, path: str) -> str:
    method = method.upper()
    if path in DESTRUCTIVE_PATHS:
        return "destructive"
    if path in CAPTURE_PATHS:
        return "capture"
    if method in {"GET", "HEAD", "OPTIONS"}:
        return "read"
    if path in {"/api/v1/auth/login", "/api/v1/auth/logout"}:
        return "authentication"
    return "mutating"


def validate_envelope(value: Any) -> Optional[str]:
    if not isinstance(value, dict):
        return "response is not a JSON object"
    missing = [key for key in ("ok", "data", "error") if key not in value]
    if missing:
        return "missing envelope fields: " + ", ".join(missing)
    if not isinstance(value["ok"], bool):
        return "envelope ok is not boolean"
    if value["ok"] and value["error"] is not None:
        return "successful response has non-null error"
    if not value["ok"] and value["error"] is None:
        return "error response has null error"
    return None


def led_payload(data: Any) -> Dict[str, Any]:
    if not isinstance(data, dict):
        return {}
    colour: Dict[str, Any] = {}
    if isinstance(data.get("colour"), dict):
        colour = dict(data["colour"])
    payload = {
        key: colour[key] for key in ("r", "g", "b") if key in colour
    }
    for key in ("brightness", "visualizer_enabled"):
        if key in data:
            payload[key] = data[key]
    return payload


def voice_pipeline_payload(data: Any) -> Dict[str, Any]:
    if not isinstance(data, dict):
        return {}
    payload: Dict[str, Any] = {}
    for key in ("mode", "configured_mode", "stt_wyoming_uri", "stt_model",
                "tts_wyoming_uri", "tts_voice"):
        if key in data and data[key] is not None:
            # The API PUT contract calls the selected mode `mode`; the GET
            # response may expose only configured/effective aliases.
            if key == "configured_mode":
                payload.setdefault("mode", data[key])
            else:
                payload[key] = data[key]
    return payload


def readback_value(data: Any, key: str) -> Any:
    if not isinstance(data, dict):
        return None
    if key in {"r", "g", "b"} and isinstance(data.get("colour"), dict):
        return data["colour"].get(key)
    return data.get(key)


def validate_semantics(path: str, data: Any) -> Optional[str]:
    if not isinstance(data, (dict, list)):
        return None
    if path.endswith("/status") and isinstance(data, dict):
        if "cpus" in data and isinstance(data["cpus"], dict):
            cores = data["cpus"].get("cores")
            if not isinstance(cores, list):
                return "status.cpus.cores is not an array"
        if "temperature_c" in data and not isinstance(data["temperature_c"], (int, float)):
            return "status.temperature_c is not numeric"
    if path.endswith("/config") and isinstance(data, dict):
        if "csrf_token" in data and not re.fullmatch(r"[0-9a-fA-F]{64}", str(data["csrf_token"])):
            return "config.csrf_token is malformed"
    if path.endswith("/events") and isinstance(data, str):
        if "event:" not in data:
            return "events response has no event marker"
    if path.endswith("/logs") and isinstance(data, dict) and "entries" in data:
        if not isinstance(data["entries"], list):
            return "logs.entries is not an array"
    return None


def error_message(value: Any) -> str:
    if not isinstance(value, dict):
        return ""
    error = value.get("error")
    if isinstance(error, dict):
        return str(error.get("message") or error.get("code") or "API error")
    return ""


def redact_secrets(value: Any) -> Any:
    if isinstance(value, dict):
        out: Dict[str, Any] = {}
        for key, item in value.items():
            out[key] = "<redacted>" if SECRET_KEYS.search(str(key)) else redact_secrets(item)
        return out
    if isinstance(value, list):
        return [redact_secrets(item) for item in value]
    return value


def parse_root_evidence(raw: str) -> Dict[str, Any]:
    evidence: Dict[str, Any] = {
        "root": bool(re.search(r"uid=0\s+gid=0", raw)),
        "kernel": "",
        "wlan0": bool(re.search(r"present /sys/class/net/wlan0", raw)),
        "wmt": bool(re.search(r"present /dev/wmt(?:\s|$)", raw)),
        "audio_socket": bool(re.search(r"present /run/libreecho/audio\.sock", raw)),
        "mic_socket": bool(re.search(r"present /run/libreecho/mic\.sock", raw)),
        "bluetooth_socket": bool(re.search(r"present /run/libreecho/bluetooth\.sock", raw)),
        "mem_total_kb": 0,
    }
    match = re.search(r"Linux\s+\S+\s+(\S+)", raw)
    if match:
        evidence["kernel"] = match.group(1)
    memory = re.search(r"MemTotal:\s+(\d+)\s+kB", raw)
    if memory:
        evidence["mem_total_kb"] = int(memory.group(1))
    evidence["adb_markers"] = {
        "start": "AUDIT_START" in raw,
        "end": "AUDIT_END" in raw,
    }
    evidence["fatal_log_lines"] = len(re.findall(r"(?im)^(?:.*(?:panic|oops|bug:|watchdog|wdt).*)$", raw))
    return evidence


def elapsed_ms(started: float) -> int:
    return int((time.monotonic() - started) * 1000)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    url_default = os.environ.get("LIBREECHO_LIVE_URL")
    adb_serial_default = os.environ.get("ADB_SERIAL")
    parser.add_argument(
        "--url",
        default=url_default,
        required=url_default is None,
        help="live device URL (or set LIBREECHO_LIVE_URL)",
    )
    parser.add_argument(
        "--adb-serial",
        default=adb_serial_default,
        required=adb_serial_default is None,
        help="ADB device serial (or set ADB_SERIAL)",
    )
    parser.add_argument("--timeout", type=float, default=8.0)
    parser.add_argument("--report", help="write redacted JSON report to this path")
    parser.add_argument("--strict-capabilities", action="store_true", help="return failure for blocked/unsupported gates")
    parser.add_argument("--capture", action="store_true", help="open the live microphone stream endpoint; may consume capture hardware")
    parser.add_argument("--mutate", action="store_true", help="reserved for reversible same-state configuration tests")
    parser.add_argument("--exercise-hardware", action="store_true", help="reserved for explicit transient hardware tests")
    args = parser.parse_args(argv)
    harness = Harness(args.url, args.timeout, args.adb_serial, args.strict_capabilities, args.capture, args.mutate, args.exercise_hardware)
    exit_code = harness.run()
    if args.report:
        report = {
            "schema": 1,
            "url": args.url,
            "adb_serial": args.adb_serial,
            "safe_default": not (args.capture or args.mutate or args.exercise_hardware),
            "results": [result.as_dict() for result in harness.results],
        }
        with open(args.report, "w", encoding="utf-8") as output:
            json.dump(redact_secrets(report), output, indent=2, sort_keys=True)
            output.write("\n")
        print(f"REPORT {args.report}")
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
