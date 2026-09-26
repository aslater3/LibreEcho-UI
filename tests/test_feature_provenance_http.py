#!/usr/bin/env python3
"""Isolated real-HTTP provenance smoke gate for the built mock daemon.

The fixture files are written with json.dump and independently parsed before the
server is started. This test never uses device paths, real users, or a LAN bind.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
DAEMON = ROOT / "build" / "libreecho-web"


def dump_json(path: Path, value: object) -> None:
    with path.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=True, separators=(",", ":"))
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    # Independent on-disk validation happens before daemon startup.
    subprocess.run(
        [sys.executable, "-m", "json.tool", str(path)],
        check=True,
        stdout=subprocess.DEVNULL,
    )
    with path.open("r", encoding="utf-8") as stream:
        assert json.load(stream) == value, f"fixture round-trip mismatch: {path}"


def sha256_bytes(path: Path, payload: bytes) -> str:
    path.write_bytes(payload)
    return hashlib.sha256(payload).hexdigest()


def free_loopback_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def http(base: str, path: str, method: str = "GET", body: object | None = None,
         headers: dict[str, str] | None = None) -> tuple[int, str, dict]:
    payload = None if body is None else json.dumps(body, separators=(",", ":")).encode()
    request_headers = {"Accept": "application/json"}
    if payload is not None:
        request_headers["Content-Type"] = "application/json"
    if headers:
        request_headers.update(headers)
    request = Request(base + path, data=payload, method=method, headers=request_headers)
    try:
        with urlopen(request, timeout=5) as response:
            status = response.status
            raw = response.read().decode("utf-8")
    except HTTPError as error:
        status = error.code
        raw = error.read().decode("utf-8")
    except URLError as error:
        raise AssertionError(f"HTTP {method} {path} failed: {error}") from error
    try:
        parsed = json.loads(raw)
    except json.JSONDecodeError as error:
        raise AssertionError(f"HTTP {method} {path} returned invalid JSON: {raw!r}") from error
    if not isinstance(parsed, dict):
        raise AssertionError(f"HTTP {method} {path} returned non-object JSON: {raw!r}")
    return status, raw, parsed


def assert_status(status: int, raw: str, expected: int, label: str) -> None:
    assert status == expected, f"{label}: expected HTTP {expected}, got {status}: {raw}"


def components(data: dict) -> list[dict]:
    value = data.get("components")
    assert isinstance(value, list) and len(value) == 5, value
    assert [item.get("feature_id") for item in value] == [
        "airplay2", "tts", "wakeword", "stt", "assistant"
    ], value
    return value


def provenance_until(base: str, auth: dict[str, str], predicate, label: str,
                     timeout: float = 5.0) -> tuple[int, str, dict]:
    deadline = time.monotonic() + timeout
    while True:
        status, raw, data = http(base, "/api/v1/provenance", headers=auth)
        assert_status(status, raw, 200, label)
        if predicate(data):
            return status, raw, data
        if time.monotonic() >= deadline:
            raise AssertionError(f"{label} did not settle: {data!r}")
        time.sleep(0.01)


def main() -> int:
    assert DAEMON.is_file() and os.access(DAEMON, os.X_OK), f"missing built daemon: {DAEMON}"
    server: subprocess.Popen[str] | None = None
    server_pid: int | None = None
    root_path: Path | None = None
    try:
        with tempfile.TemporaryDirectory(prefix="libreecho-http-smoke-") as temp:
            root_path = Path(temp)
            feature_root = root_path / "features"
            update_root = root_path / "update"
            run_root = root_path / "run"
            feature_root.mkdir()
            (feature_root / "tts").mkdir()
            (feature_root / "airplay2").mkdir()
            (update_root / "staging" / "features" / "tts").mkdir(parents=True)
            run_root.mkdir()

            config_path = root_path / "web-config.json"
            mock_config_path = root_path / "mock-state.json"
            users_path = root_path / "users"
            dump_json(config_path, {})
            dump_json(mock_config_path, {
                "networks": [
                    {"ssid": "FixtureNet", "security": "wpa2", "signal": 71, "connect": "success"}
                ],
                "temperature_c": 47,
                "seed": 12345,
            })
            malformed_path = root_path / "malformed-fixture.json"
            dump_json(malformed_path, {"valid": True})
            malformed_path.write_text(
                malformed_path.read_text(encoding="utf-8")[:-2], encoding="utf-8"
            )
            malformed_check = subprocess.run(
                [sys.executable, "-m", "json.tool", str(malformed_path)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            assert malformed_check.returncode != 0
            malformed_path.unlink()
            print("malformed_fixture_validation: rejected independently")

            base_payload = b"LibreEcho HTTP canonical base payload\n"
            runtime_payload = b"LibreEcho HTTP canonical runtime capsule\n"
            staged_runtime = b"LibreEcho HTTP staged runtime candidate\n"
            replacement_payload = b"LibreEcho HTTP staged full replacement\n"
            base_hash = sha256_bytes(feature_root / "tts" / "payload.squashfs", base_payload)
            runtime_hash = sha256_bytes(feature_root / "tts" / "runtime.squashfs", runtime_payload)
            staged_runtime_hash = sha256_bytes(
                root_path / "update" / "staging" / "features" / "tts" /
                "libreecho-radar-puffin-v0.13.11-tts.runtime.squashfs", staged_runtime
            )
            replacement_hash = hashlib.sha256(replacement_payload).hexdigest()
            daemon_hash = sha256_bytes(
                root_path / "libreecho-ttsd", b"LibreEcho HTTP smoke daemon fixture\n"
            )
            legacy_payload = b"LibreEcho HTTP canonical legacy payload\n"
            legacy_hash = sha256_bytes(feature_root / "airplay2" / "payload.squashfs", legacy_payload)
            release = 'radar-puffin-v0.13.11"fixture'
            source = "0123456789012345678901234567890123456789"
            runtime_manifest = {
                "schema_version": 1,
                "feature_id": "tts",
                "product_release": release,
                "source_commit": source,
                "base_payload_sha256": base_hash,
                "payload": {
                    "filename": "runtime.squashfs",
                    "sha256": runtime_hash,
                    "size": len(runtime_payload),
                },
                "files": {
                    "usr/local/sbin/libreecho-ttsd": {"sha256": daemon_hash}
                },
            }
            dump_json(feature_root / "tts" / "runtime-manifest.json", runtime_manifest)
            dump_json(feature_root / "airplay2" / "manifest.json", {
                "schema_version": 1,
                "feature_id": "airplay2",
                "format": "squashfs-lz4",
                "payload": {
                    "filename": "airplay2.squashfs",
                    "sha256": legacy_hash,
                    "size": len(legacy_payload),
                },
                "files": {},
            })
            (run_root / "libreecho-ttsd.pid").write_text("pending\n", encoding="ascii")
            dump_json(update_root / "staging" / "candidate-fixture.json", {
                "candidate": staged_runtime_hash,
            })
            (update_root / "staging" / "manifest").write_text(
                "feature_tts_action=runtime\n"
                "feature_tts_asset=libreecho-radar-puffin-v0.13.11-tts.runtime.squashfs\n"
                f"feature_tts_sha256={staged_runtime_hash}\n"
                "feature_tts_activation=reboot\n", encoding="ascii"
            )
            (update_root / "feature-commit").write_text("transaction_id=txn-live\nphase=prepared\n", encoding="ascii")
            print("fixture_json_validation: ok (json.dump + python -m json.tool)")

            port = free_loopback_port()
            base_url = f"http://127.0.0.1:{port}"
            log_path = root_path / "server.log"
            log = log_path.open("w", encoding="utf-8")
            try:
                server = subprocess.Popen(
                    [
                        str(DAEMON), "--backend", "mock", "--config", str(config_path),
                        "--mock-config", str(mock_config_path), "--web-root", str(ROOT / "web"),
                        "--listen", f"127.0.0.1:{port}", "--seed", "42",
                        "--users-file", str(users_path),
                    ],
                    cwd=ROOT,
                    env={
                        **os.environ,
                        "LIBREECHO_FEATURE_ROOT": str(feature_root),
                        "LIBREECHO_UPDATE_ROOT": str(update_root),
                        "LIBREECHO_FEATURE_RUN_ROOT": str(run_root),
                    },
                    stdout=log,
                    stderr=subprocess.STDOUT,
                    text=True,
                )
                server_pid = server.pid
                (run_root / "libreecho-ttsd.pid").write_text(f"{server.pid}\n", encoding="ascii")
            finally:
                log.close()

            deadline = time.monotonic() + 5
            while True:
                try:
                    status, _, config = http(base_url, "/api/v1/config")
                    if status == 200:
                        break
                except AssertionError:
                    pass
                if time.monotonic() >= deadline:
                    server_log = log_path.read_text(encoding="utf-8", errors="replace")
                    raise AssertionError(f"daemon did not become ready; log={server_log!r}")
                time.sleep(0.05)
            assert config["ok"] is True
            csrf = config["data"]["csrf_token"]
            assert re.fullmatch(r"[0-9a-fA-F]{64}", csrf), csrf
            assert config["data"]["authentication"] == "bootstrap-required"

            status, raw, denied = http(base_url, "/api/v1/provenance")
            assert_status(status, raw, 401, "unauthenticated provenance")
            assert denied == {
                "ok": False,
                "data": None,
                "error": {"code": "forbidden", "message": "Initial account setup is required"},
            }, denied
            print(f"GET /api/v1/provenance unauthenticated: HTTP {status}; exact forbidden envelope")

            status, raw, bootstrap = http(
                base_url, "/api/v1/auth/bootstrap", "POST",
                {"username": "admin", "password": "fixture-password-123",
                 "password_confirm": "fixture-password-123"},
                {"X-LibreEcho-CSRF": csrf},
            )
            assert_status(status, raw, 200, "documented CSRF bootstrap")
            assert bootstrap["ok"] is True
            assert bootstrap["data"]["username"] == "admin"
            token = bootstrap["data"]["token"]
            assert isinstance(token, str) and len(token) >= 16
            auth = {"Authorization": f"Bearer {token}"}
            print(
                f"POST /api/v1/auth/bootstrap CSRF: HTTP {status}; "
                f"username=admin token_length={len(token)}"
            )

            setup_payload = {
                "hostname": "fixture-echo",
                "ssid": "FixtureNet",
                "security": "wpa2",
                "password": "fixture-password-123",
                "volume": 52,
                "wake_word": "LibreEcho",
                "wake_sensitivity": 72,
                "local_only": True,
                "diagnostic_telemetry": False,
            }
            status, raw, setup = http(
                base_url, "/api/v1/setup", "POST", setup_payload,
                {**auth, "X-LibreEcho-CSRF": csrf},
            )
            assert_status(status, raw, 200, "setup completion")
            assert setup["ok"] is True
            status, raw, post_setup_config = http(base_url, "/api/v1/config", headers=auth)
            assert_status(status, raw, 200, "post-setup config")
            assert post_setup_config["data"]["setup_completed"] is True
            assert (Path(str(config_path) + ".setup-complete")).is_file()
            assert users_path.is_file() and (users_path.stat().st_mode & 0o777) == 0o600
            print("POST /api/v1/setup: HTTP 200; setup_completed=true; marker=present")

            expected_tts = {
                "feature_id": "tts", "release": "unavailable", "source_commit": "unavailable",
                "effective_payload_sha256": base_hash, "runtime_capsule_sha256": runtime_hash,
                "candidate_kind": "runtime", "candidate_payload_sha256": staged_runtime_hash,
                "candidate_status": "present",
                "running_daemon_sha256": hashlib.sha256(DAEMON.read_bytes()).hexdigest(),
                "running_daemon_status": "running", "effective": "present",
                "activation": "reboot", "last_transaction_result": "commit-pending",
            }
            expected_legacy = {
                "feature_id": "airplay2", "release": "unavailable", "source_commit": "unavailable",
                "effective_payload_sha256": legacy_hash, "runtime_capsule_sha256": None,
                "candidate_kind": "unavailable", "candidate_payload_sha256": "unavailable",
                "candidate_status": "missing",
                "running_daemon_sha256": "not-running", "running_daemon_status": "not-running",
                "effective": "present", "activation": "unavailable", "last_transaction_result": "commit-pending",
            }

            status, raw, provenance = provenance_until(
                base_url, auth,
                lambda data: components(data["data"])[0]["effective"] == "present" and
                components(data["data"])[1]["effective"] == "present" and
                components(data["data"])[1]["candidate_status"] == "present",
                "authenticated provenance",
            )
            assert provenance["ok"] is True
            provenance_components = components(provenance["data"])
            assert provenance["data"]["transaction_state"] == "commit"
            assert provenance["data"]["last_transaction_result"] == "commit-pending"
            assert provenance_components[0] == expected_legacy
            assert provenance_components[1] == expected_tts, provenance_components[1]
            assert all(item["runtime_capsule_sha256"] is None for item in provenance_components[2:])
            assert all(item["effective"] == "missing" for item in provenance_components[2:])

            runtime_manifest_path = feature_root / "tts" / "runtime-manifest.json"
            oversized_runtime_manifest = b"{" + b" " * (65536 - 2) + b"}"
            assert len(oversized_runtime_manifest) == 65536
            runtime_manifest_path.write_bytes(oversized_runtime_manifest)
            assert json.loads(runtime_manifest_path.read_text(encoding="ascii")) == {}
            status, raw, oversized = provenance_until(
                base_url, auth,
                lambda data: components(data["data"])[1]["effective"] == "unavailable" and
                components(data["data"])[1]["release"] == "unavailable",
                "oversized runtime metadata",
            )
            oversized_tts = components(oversized["data"])[1]
            assert oversized_tts["effective_payload_sha256"] == "unavailable"
            assert oversized_tts["runtime_capsule_sha256"] is None

            runtime_manifest_path.unlink()
            status, raw, missing_runtime_metadata = provenance_until(
                base_url, auth,
                lambda data: components(data["data"])[1]["effective"] == "unavailable" and
                components(data["data"])[1]["release"] == "unavailable",
                "runtime capsule without metadata",
            )
            missing_metadata_tts = components(missing_runtime_metadata["data"])[1]
            assert missing_metadata_tts["effective_payload_sha256"] == "unavailable"
            assert missing_metadata_tts["runtime_capsule_sha256"] is None
            dump_json(runtime_manifest_path, runtime_manifest)
            print(
                "runtime metadata bounds: exact 65536-byte manifest and missing metadata "
                "with runtime capsule fail closed"
            )
            print(
                "GET /api/v1/provenance authenticated: HTTP 200; "
                f"tts exact hashes base={base_hash} runtime={runtime_hash} daemon={daemon_hash}; "
                "legacy/missing identity unavailable"
            )

            canonical_base_path = feature_root / "tts" / "payload.squashfs"
            canonical_base_tampered = b"X" + base_payload[1:]
            canonical_base_path.write_bytes(canonical_base_tampered)
            status, raw, tampered = provenance_until(
                base_url, auth,
                lambda data: components(data["data"])[1]["effective"] == "mismatch",
                "same-size canonical tamper",
            )
            tampered_tts = components(tampered["data"])[1]
            assert tampered_tts["effective"] == "mismatch"
            assert tampered_tts["effective_payload_sha256"] == "unavailable"
            canonical_base_path.unlink()
            status, raw, missing = provenance_until(
                base_url, auth,
                lambda data: components(data["data"])[1]["effective"] == "missing",
                "missing canonical payload",
            )
            missing_tts = components(missing["data"])[1]
            assert missing_tts["effective"] == "missing"
            assert missing_tts["effective_payload_sha256"] == "unavailable"
            canonical_base_path.write_bytes(base_payload)

            staged_path = (update_root / "staging" / "features" / "tts" /
                           "libreecho-radar-puffin-v0.13.11-tts.runtime.squashfs")
            staged_tampered = b"Y" + staged_runtime[1:]
            staged_path.write_bytes(staged_tampered)
            status, raw, staged_mismatch = provenance_until(
                base_url, auth,
                lambda data: components(data["data"])[1]["candidate_status"] == "mismatch" and
                components(data["data"])[1]["effective"] == "present",
                "same-size staged tamper",
            )
            staged_tts = components(staged_mismatch["data"])[1]
            assert staged_tts["effective"] == "present", staged_tts
            assert staged_tts["candidate_kind"] == "runtime"
            assert staged_tts["candidate_status"] == "mismatch"
            assert staged_tts["candidate_payload_sha256"] == hashlib.sha256(staged_tampered).hexdigest()
            staged_path.unlink()
            replacement_name = "libreecho-radar-puffin-v0.13.11-tts.payload.squashfs"
            replacement_path = update_root / "staging" / "features" / "tts" / replacement_name
            replacement_path.write_bytes(replacement_payload)
            (update_root / "staging" / "manifest").write_text(
                "feature_tts_action=replace\n"
                f"feature_tts_asset={replacement_name}\n"
                f"feature_tts_sha256={replacement_hash}\n"
                "feature_tts_activation=reboot\n", encoding="ascii"
            )
            status, raw, different_candidate = provenance_until(
                base_url, auth,
                lambda data: components(data["data"])[1]["candidate_status"] == "present" and
                components(data["data"])[1]["candidate_kind"] == "replacement",
                "different canonical and candidate",
            )
            different_tts = components(different_candidate["data"])[1]
            assert different_tts["effective"] == "present"
            assert different_tts["runtime_capsule_sha256"] == runtime_hash
            assert different_tts["candidate_kind"] == "replacement"
            assert different_tts["candidate_status"] == "present"
            assert different_tts["candidate_payload_sha256"] == replacement_hash
            print("GET /api/v1/provenance identity adversaries: tampered/missing/staged/replacement cases rejected or distinguished")

            (feature_root / "assistant").mkdir()
            large_manifest = feature_root / "assistant" / "manifest.json"
            dump_json(large_manifest, {
                "schema_version": 1,
                "feature_id": "assistant",
                "product_release": "radar-puffin-v0.13.11",
                "source_commit": source,
                "payload": {
                    "filename": "assistant.squashfs",
                    "sha256": "0" * 64,
                    "size": 512 * 1024 * 1024,
                },
                "files": {},
            })
            large_payload = feature_root / "assistant" / "payload.squashfs"
            with large_payload.open("wb") as stream:
                stream.truncate(512 * 1024 * 1024)
            started = time.monotonic()
            status, raw, cold = http(base_url, "/api/v1/provenance", headers=auth)
            cold_elapsed = time.monotonic() - started
            assert_status(status, raw, 200, "cold large provenance")
            cold_assistant = components(cold["data"])[4]
            assert cold_assistant["effective"] == "pending", cold_assistant
            assert cold_elapsed < 1.0, cold_elapsed
            started = time.monotonic()
            status, raw, responsive = http(base_url, "/api/v1/config", headers=auth)
            responsive_elapsed = time.monotonic() - started
            assert_status(status, raw, 200, "responsive config during provenance hash")
            assert responsive["ok"] is True
            assert responsive_elapsed < 1.0, responsive_elapsed
            provenance_components = components(cold["data"])
            print(
                "responsiveness: cold 512 MiB provenance returned pending in "
                f"{cold_elapsed:.3f}s; config remained serviceable in {responsive_elapsed:.3f}s"
            )

            status, raw, ota = http(base_url, "/api/v1/system/update", headers=auth)
            assert_status(status, raw, 200, "OTA status")
            assert ota["ok"] is True
            assert ota["data"]["supported"] is False
            assert ota["data"]["state"] == "idle"
            assert ota["data"]["components"] == provenance_components, (ota["data"]["components"], provenance_components)
            assert ota["data"]["transaction_state"] == "commit"
            assert ota["data"]["last_transaction_result"] == "commit-pending"
            print("GET /api/v1/system/update: HTTP 200; supported=false; exact feature payload/runtime fields")

            status, raw, diagnostic = http(
                base_url, "/api/v1/diagnostics/export", "POST", {},
                {**auth, "X-LibreEcho-CSRF": csrf},
            )
            assert_status(status, raw, 200, "diagnostic export")
            assert diagnostic["ok"] is True
            diagnostic_data = diagnostic["data"]
            assert diagnostic_data["format"] == "libreecho-diagnostic-bundle"
            assert diagnostic_data["release_identity"]["components"] == provenance_components
            assert diagnostic_data["release_identity"]["transaction_state"] == "commit"
            assert diagnostic_data["release_identity"]["last_transaction_result"] == "commit-pending"
            print("POST /api/v1/diagnostics/export: HTTP 200; exact feature payload/runtime fields")
            # With no live/current record, invalid rollback evidence is unknown.
            # Platform copies pending verbatim; a valid rollback retains prepared.
            (update_root / "feature-commit").unlink()
            for record, expected_state, expected_result in (
                ("", "unknown", "unknown"),
                ("not a transaction record\n", "unknown", "unknown"),
                ("transaction_id=txn-failed\nphase=corrupt\n", "unknown", "unknown"),
                ("schema=2\ntransaction_id=txn-failed\nphase=prepared\n", "rollback", "rolled-back"),
            ):
                (update_root / "rolled-back").write_text(record)
                for endpoint in ("/api/v1/provenance", "/api/v1/system/update", "/api/v1/diagnostics/export"):
                    exporting = endpoint.endswith("/export")
                    status, raw, response = http(
                        base_url, endpoint, "POST" if exporting else "GET",
                        {} if exporting else None,
                        {**auth, "X-LibreEcho-CSRF": csrf} if exporting else auth,
                    )
                    assert_status(status, raw, 200, "rollback record validation")
                    data = response["data"]["release_identity"] if exporting else response["data"]
                    assert data["transaction_state"] == expected_state, data
                    assert data["last_transaction_result"] == expected_result, data
                    assert all(item["last_transaction_result"] == expected_result for item in components(data))
            print("rollback records: malformed/empty remain unknown; copied prepared record reports rollback across all three APIs")
            print("http_smoke: ok")
    finally:
        if server is not None and server.poll() is None:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)
        if server_pid is not None:
            try:
                os.kill(server_pid, 0)
            except ProcessLookupError:
                process_absent = True
            except PermissionError:
                process_absent = False
            else:
                process_absent = False
        else:
            process_absent = True
    assert process_absent, f"daemon process still exists: {server_pid}"
    assert root_path is not None
    # TemporaryDirectory has removed the entire fixture/config root now.
    assert not root_path.exists(), root_path
    print(f"cleanup: process_absent=true root_absent=true root={root_path}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"http_smoke: FAIL: {error}", file=sys.stderr)
        raise
