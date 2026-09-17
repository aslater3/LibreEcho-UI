#!/usr/bin/env python3
"""Real timerd + watchdog + shipped init: a crash must retain pending timers.

The watchdog is started with the Web daemon's identity (ARGS, DAEMON, PIDFILE,
LOGFILE) in its environment, the way the Web backend carries its own. None of
it belongs to the service being recovered, so the restart must still reach the
shipped init script with the timer's own configuration -- the launcher below
is that configuration, standing in for /etc/default/libreecho-timerd.
"""
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
        audio = root / "absent-audio.sock"
        received = root / "recovered-environment"
        # The caller's identity: what the Web daemon carries while it controls
        # other services. A recovery must not turn any of it into the timer's
        # own configuration (issue #249).
        env = {**os.environ,
               "ARGS": "--backend linux --config /data/libreecho/config/web-config.json "
                       "--web-root /usr/local/share/libreecho/web --listen 0.0.0.0:8080",
               "DAEMON": str(root / "libreecho-web"),
               "PIDFILE": str(root / "libreecho-web.pid"),
               "LOGFILE": str(root / "libreecho-web.log")}
        launcher = root / "timer.init"
        launcher.write_text(
            "#!/bin/sh\n"
            f'printf "%s\\n" "$1" >> {shlex.quote(str(marker))}\n'
            "{\n"
            '    printf "ARGS=%s\\n" "${ARGS-<unset>}"\n'
            '    printf "DAEMON=%s\\n" "${DAEMON-<unset>}"\n'
            '    printf "PIDFILE=%s\\n" "${PIDFILE-<unset>}"\n'
            '    printf "LOGFILE=%s\\n" "${LOGFILE-<unset>}"\n'
            f"}} >> {shlex.quote(str(received))}\n"
            f"DAEMON={shlex.quote(str(daemon))} SOCKET={shlex.quote(str(sock))} \\\n"
            f"STATE={shlex.quote(str(state))} PIDFILE={shlex.quote(str(pidfile))} \\\n"
            f"LOGFILE={shlex.quote(str(logfile))} \\\n"
            f"AUDIO_SOCKET={shlex.quote(str(audio))} \\\n"
            f'exec sh {shlex.quote(str(ROOT / "init/libreecho-timerd.init"))} "$1"\n')
        timer = watchdog = None
        try:
            with logfile.open("w") as output:
                timer = subprocess.Popen([str(daemon), "--foreground", "--socket", str(sock),
                                          "--state", str(state), "--audio-socket",
                                          str(audio)], stdout=output, stderr=output)
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
            # The recovery reached the init script without the watchdog's own
            # ARGS/DAEMON/PIDFILE/LOGFILE, so the timer resolved its own.
            recovered = received.read_text().splitlines()
            assert recovered, "the recovery never ran the init script"
            assert all(line.endswith("=<unset>") for line in recovered), recovered
            assert {line.split("=", 1)[0] for line in recovered} == {
                "ARGS", "DAEMON", "PIDFILE", "LOGFILE"}, recovered
            print("watchdog timer recovery: healthy service spared; crash restarted once; "
                  "schedule retained; caller identity kept out of the restart")
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
