#!/usr/bin/env python3
"""Bounded, host-isolated ESPHome control-plane checks; no image/hardware work."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import socket
from esphome_fixture_build import daemon_binary

ROOT = Path(__file__).resolve().parents[1]
SCRATCH = Path(os.environ.get("TMPDIR", ROOT / "build"))
SCRATCH.mkdir(parents=True, exist_ok=True)


def run(argv, timeout=90, env=None):
    subprocess.run(argv, cwd=ROOT, check=True, timeout=timeout, env=env)


with tempfile.TemporaryDirectory(prefix="test-esphome-control-", dir=SCRATCH) as directory:
    fixture = Path(directory)
    flags = [os.environ.get("CC", "cc"), "-D_POSIX_C_SOURCE=200809L", "-std=c99", "-O2",
             "-Wall", "-Wextra", "-Wpedantic", "-Wno-unused-variable", "-Wno-unused-function",
             "-Wno-format-truncation", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
             '-DLE_SOURCE_COMMIT="host-test"', '-DLE_SOURCE_DIRTY="1"', '-DLE_SOURCE_DIGEST="host-test"',
             "-Isrc", "-Isrc/adapter"]
    binary = fixture / "control"
    run(flags + ['-DLE_TEST_ROOT="' + str(fixture) + '"', "tests/test_esphome_control.c",
                 "src/backend.c", "src/json.c", "src/config_store.c", "src/service_env.c",
                 "src/event_bus.c", "src/log.c", "src/adapter/wyoming_client.c", "-o", str(binary)])
    daemon = daemon_binary(fixture)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    idme = fixture / "idme/mac_addr"
    idme.mkdir(parents=True)
    (idme / "value").write_text("02:00:00:00:00:01")
    (fixture / "privacy").write_text("0\n")
    env = dict(os.environ, LE_TEST_ESPHOMED_BIN=str(daemon),
               LE_TEST_ESPHOMED_INIT=str(ROOT / "init/libreecho-esphomed.init"),
               LIBREECHO_ESPHOMED_DAEMON=str(daemon), LIBREECHO_ESPHOME_PORT=str(port))
    stop_env = dict(env, DAEMON=str(daemon), DEFAULTS=str(fixture / "no-defaults"),
                    PIDFILE=str(fixture / "esphomed.pid"), STATUS_FILE=str(fixture / "status.json"), PORT=str(port))
    cases = sys.argv[1:] or ["protocol", "secrets", "transitions", "wake-save"]
    for case in cases:
        if case not in ("protocol", "secrets", "transitions", "wake-save"):
            raise SystemExit("Unknown control-plane test: " + case)
        try:
            run([str(binary), case], timeout=45, env=env)
        finally:
            # Preserve creator identity before a negative case tampers with status/pid.
            if (fixture / "creator.pid").exists():
                (fixture / "esphomed.pid").write_bytes((fixture / "creator.pid").read_bytes())
                (fixture / "status.json").write_bytes((fixture / "creator.json").read_bytes())
            run(["sh", str(ROOT / "init/libreecho-esphomed.init"), "stop"], timeout=10, env=stop_env)
            for name in ("creator.pid", "creator.json"):
                (fixture / name).unlink(missing_ok=True)
    redaction = fixture / "redaction"
    run(flags + ["tests/test_esphome_control_redaction.c", "-o", str(redaction)])
    run([str(redaction)])
print("ESPHome control-plane fixture cleanup: PASS")
