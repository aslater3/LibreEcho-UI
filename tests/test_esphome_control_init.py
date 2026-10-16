#!/usr/bin/env python3
"""Exercise shipped boot readiness against actual daemon/process/listener evidence."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from esphome_fixture_build import daemon_binary

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="esphome-boot-health-", dir=os.environ.get("TMPDIR")) as directory:
    daemon = daemon_binary(directory)
    env = dict(os.environ, ESPHOMED_BIN=str(daemon), PYTHONDONTWRITEBYTECODE="1")
    subprocess.run([sys.executable, "tests/test_esphome_health.py",
                    "HealthFixture.test_boot_init_rejects_unrelated_live_pid",
                    "HealthFixture.test_stale_process_identity_and_unrelated_listener_fail_closed",
                    "HealthFixture.test_actual_daemon_authenticated_connection_and_disconnect"],
                   env=env, cwd=ROOT, check=True, timeout=45)
print("Web boot ESPHome real executable/start/status/listener evidence and cleanup: PASS")
