#!/usr/bin/env python3
"""Exercise the shipped HTTP surface with private mock configuration and loopback."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SCRATCH = Path(os.environ.get("TMPDIR", ROOT / "build"))
SCRATCH.mkdir(parents=True, exist_ok=True)
KEY = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="  # explicit, public test fixture
with tempfile.TemporaryDirectory(prefix="test-esphome-http-", dir=SCRATCH) as directory:
    fixture = Path(directory)
    config = fixture / "config.json"
    config.write_text(json.dumps({"voice_pipeline_mode": "custom", "integrations": 0,
                                 "ha_protocol": "wyoming", "esphome_noise_key": KEY,
                                 "stt_wyoming_uri": "tcp://127.0.0.1:10300",
                                 "stt_wyoming_model": "saved-whisper",
                                 "tts_wyoming_uri": "tcp://127.0.0.1:10200",
                                 "tts_wyoming_voice": "saved-piper",
                                 "wake_word": "HeyJarvis", "wake_sensitivity": 55,
                                 "button_action_brightness": 37,
                                 "button_long": "Open pairing mode",
                                 "mac_wifi": "02:11:22:33:44:55"}))
    config.chmod(0o600)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    base = "http://127.0.0.1:" + str(port)
    env = dict(os.environ, LIBREECHO_ESPHOMED_PIDFILE=str(fixture / "esphomed.pid"),
               LIBREECHO_ESPHOME_STATUS_FILE=str(fixture / "status.json"))
    with (fixture / "server.log").open("w") as log:
        server = subprocess.Popen([os.environ.get("LIBREECHO_WEB_BIN", str(ROOT / "build/libreecho-web")), "--backend", "mock",
                                   "--mock-config", str(ROOT / "config/mock-state.json"),
                                   "--config", str(config), "--web-root", str(ROOT / "web"),
                                   "--listen", "127.0.0.1:" + str(port), "--seed", "42", "--dev-controls"],
                                  cwd=ROOT, env=env, stdout=log, stderr=log)
        try:
            csrf = ""
            def request(path, body=None, method=None, authorized=True):
                data = json.dumps(body).encode() if body is not None else None
                headers = {"Content-Type": "application/json"}
                if csrf and authorized:
                    headers["X-LibreEcho-CSRF"] = csrf
                req = urllib.request.Request(base + "/api/v1" + path, data=data, headers=headers,
                                             method=method or ("PUT" if data else "GET"))
                try:
                    with urllib.request.urlopen(req, timeout=3) as result:
                        return result.status, json.loads(result.read())
                except urllib.error.HTTPError as error:
                    return error.code, json.loads(error.read())
            for attempt in range(60):
                if server.poll() is not None:
                    raise RuntimeError("mock server exited during startup")
                try:
                    status, result = request("/config")
                    if status == 200:
                        csrf = result["data"]["csrf_token"]
                        break
                except urllib.error.URLError:
                    time.sleep(0.05)
            else:
                raise RuntimeError("mock HTTP server did not become ready")
            assert request("/integrations/home-assistant", {"enabled": True}, authorized=False)[0] == 403
            for mode in ("local", "custom"):
                assert request("/voice-pipeline", {"mode": mode})[0] == 200
                assert request("/integrations/home-assistant", {"enabled": True})[0] == 200
                status, result = request("/voice-pipeline")
                assert status == 200 and result["data"]["mode"] == "home-assistant"
                assert result["data"]["home_assistant"] == {"protocol": "esphome", "port": 6053,
                                                            "ready": False, "connected": False}
                stored = json.loads(config.read_text())
                assert stored["ha_protocol"] == "esphome" and stored["esphome_noise_key"] == KEY
                assert stored["voice_pipeline_previous_mode"] == mode
                assert stored["button_action_brightness"] == 37
                assert stored["button_long"] == "Open pairing mode"
                assert stored["mac_wifi"] == "02:11:22:33:44:55"
                assert request("/privacy", {"local_only": True})[0] == 409
                assert request("/integrations/home-assistant", {"enabled": False})[0] == 200
                data = request("/voice-pipeline")[1]["data"]
                assert data["mode"] == mode and data["stt"]["model"] == "saved-whisper"
                assert data["tts"]["voice"] == "saved-piper"
                assert json.loads(config.read_text())["integrations"] & 1 == 0
            # Ordinary unrelated HTTP saves must retain all three HA wake states,
            # without replacing the independent Local/Custom selection.
            for selected in (None, "", "alexa_v0.1"):
                stored = json.loads(config.read_text())
                if selected is None:
                    stored.pop("esphome_active_wake_word", None)
                else:
                    stored["esphome_active_wake_word"] = selected
                config.write_text(json.dumps(stored))
                for enabled in (True, False):
                    assert request("/integrations/mqtt", {"enabled": enabled})[0] == 200
                    loaded = json.loads(config.read_text())
                    assert ("esphome_active_wake_word" in loaded) == (selected is not None)
                    if selected is not None:
                        assert loaded["esphome_active_wake_word"] == selected
                    assert loaded["wake_word"] == "HeyJarvis" and loaded["wake_sensitivity"] == 55
            for path in ("/config", "/config/export", "/voice-pipeline", "/diagnostics/export", "/logs"):
                status, result = request(path, body={} if path == "/diagnostics/export" else None,
                                         method="POST" if path == "/diagnostics/export" else "GET")
                assert status == 200, (path, status)
                encoded = json.dumps(result)
                assert KEY not in encoded and "esphome_noise_key" not in encoded, path
            assert request("/voice-pipeline", method="POST")[0] == 405
            assert config.stat().st_mode & 0o777 == 0o600
            assert Path(str(config) + ".bak").stat().st_mode & 0o777 == 0o600
            # The historical shell test additionally requires curl and jq;
            # keep the stdlib HTTP contract independently runnable on build hosts.
            if "--existing-transitions" in sys.argv[1:]:
                subprocess.run(["sh", "tests/test_voice_pipeline_ha_transitions.sh"], cwd=ROOT,
                               env=dict(env, LIBREECHO_TEST_URL=base, LIBREECHO_TEST_CONFIG=str(config)),
                               check=True, timeout=30)
            print("HTTP Local/Custom <-> ESPHome, CSRF, persisted settings and secret redaction: PASS")
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait(timeout=5)
print("ESPHome HTTP server and fixture cleanup: PASS")
