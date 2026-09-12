#!/usr/bin/env python3
"""Real timerd + watchdog + shipped init: a crash must retain pending timers."""
from __future__ import annotations

import json
import os
import shlex
import shutil
import signal
import socket
import subprocess
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def wait_for(predicate, message: str, seconds: float = 10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, ValueError):
            pass
        time.sleep(0.05)
    raise AssertionError(message)


def call(path: Path, command: str, args: dict | None = None):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
        client.settimeout(1)
        client.connect(str(path))
        client.sendall((json.dumps({"v": 1, "id": 1, "cmd": command,
                                   "args": args or {}}) + "\n").encode())
        with client.makefile("rb") as response:
            result = json.loads(response.readline(65536))
        assert result["ok"], result
        return result["data"]


def main():
    with tempfile.TemporaryDirectory(prefix="libreecho-timer-watchdog-") as temp:
        root = Path(temp)
        # A private executable path also isolates the init script's process sweep.
        daemon = root / "timerd"
        shutil.copy2(ROOT / "build/libreecho-timerd", daemon)
        sock, state = root / "timer.sock", root / "timers"
        pidfile, logfile = root / "timer.pid", root / "timer.log"
        marker, wdlog = root / "actions", root / "watchdog.log"
        env = {**os.environ, "DAEMON": str(daemon), "SOCKET": str(sock),
               "STATE": str(state), "AUDIO_SOCKET": str(root / "absent-audio.sock"),
               "PIDFILE": str(pidfile), "LOGFILE": str(logfile)}
        env.pop("ARGS", None)
        launcher = root / "timer.init"
        launcher.write_text("#!/bin/sh\n"
                            f'printf "%s\\n" "$1" >> {shlex.quote(str(marker))}\n'
                            f'exec sh {shlex.quote(str(ROOT / "init/libreecho-timerd.init"))} "$1"\n')
        timer = watchdog = None
        try:
            with logfile.open("w") as output:
                timer = subprocess.Popen([str(daemon), "--foreground", "--socket", str(sock),
                                          "--state", str(state), "--audio-socket",
                                          env["AUDIO_SOCKET"]], stdout=output, stderr=output)
            pidfile.write_text(str(timer.pid) + "\n")
            wait_for(lambda: call(sock, "status"), "timer did not become ready")
            call(sock, "add", {"seconds": 600, "label": "persist-through-crash"})
            wait_for(lambda: state.exists() and state.stat().st_size,
                     "timer did not persist its schedule")
            with wdlog.open("w") as output:
                watchdog = subprocess.Popen(
                    [str(ROOT / "build/libreecho-watchdogd"), "--interval", "1", "--passes", "12",
                     "--service", f"timerd:{sock}:{launcher}"],
                    env=env, stdout=output, stderr=output)
            wait_for(lambda: "supervising timerd" in wdlog.read_text(),
                     "watchdog never observed the healthy timer")
            time.sleep(1.1)
            assert not marker.exists(), "healthy timer was unnecessarily restarted"
            timer.kill()
            timer.wait(timeout=2)
            original_pid = timer.pid
            wait_for(lambda: pidfile.exists() and int(pidfile.read_text()) != original_pid,
                     "watchdog did not restart the timer")
            restored = wait_for(lambda: call(sock, "status"), "restarted timer is not answering")
            assert "persist-through-crash" in json.dumps(restored), restored
            time.sleep(2.1)
            assert marker.read_text().splitlines() == ["stop", "start"], marker.read_text()
            assert "giving up" not in wdlog.read_text(), wdlog.read_text()
            print("watchdog timer recovery: healthy service spared; crash restarted once; schedule retained")
        finally:
            if watchdog is not None:
                watchdog.terminate()
                try:
                    watchdog.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    watchdog.kill()
                    watchdog.wait(timeout=3)
            if timer is not None and timer.poll() is None:
                timer.kill()
                timer.wait(timeout=3)
            if pidfile.exists():
                try:
                    pid = int(pidfile.read_text())
                    if pid > 1 and Path(f"/proc/{pid}/exe").resolve() == daemon:
                        os.kill(pid, signal.SIGTERM)
                        deadline = time.monotonic() + 3
                        while Path(f"/proc/{pid}/exe").resolve() == daemon:
                            if time.monotonic() > deadline:
                                os.kill(pid, signal.SIGKILL)
                            time.sleep(0.05)
                except (OSError, ValueError):
                    pass


if __name__ == "__main__":
    main()
