#!/usr/bin/env python3
"""Rootless virtual LibreEcho service environment.

This is deliberately a service/API harness, not an MT8163 emulator.  It
provides the Unix-socket companion-daemon contracts with deterministic state
and runs libreecho-web against them in an isolated bwrap namespace.
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

SERVICES = {
    "network": {
        "status": {"state": "connected", "connectivity": "healthy", "gateway_reachable": True,
                    "internet": True, "ssid": "VirtualEchoNet", "ip": "192.0.2.42",
                    "hostname": "libreecho-virtual", "signal": 78},
        "scan": {"networks": [{"ssid": "VirtualEchoNet", "security": "wpa2", "signal": 78},
                                {"ssid": "VirtualGuest", "security": "open", "signal": 51}]},
    },
    "audio": {"status": {"volume": 55, "microphone_gain": 50, "muted": False, "playing": False}},
    "mic": {"status": {"available": True, "simulated": True, "channels": 7, "sample_rate": 16000}},
    "led": {"status": {"colour": {"r": 0, "g": 64, "b": 255}, "brightness": 50,
                         "visualizer_enabled": False, "visualizer_owner": "none", "visualizer_mood": "idle",
                         "profiles": []}},
    "bluetooth": {"status": {"state": "up", "enabled": True, "hci": "virtual-hci0",
                               "discoverable": False, "connectable": True, "pairing": False,
                               "devices": [], "profiles": ["a2dp-sink", "avrcp"]}},
    "airplay": {"status": {"state": "ready", "enabled": True, "rtsp": True, "mdns": True}},
    "wakeword": {"status": {"enabled": True, "wake_word": "alexa", "sensitivity": 60,
                              "detected_count": 0, "simulated": True}},
    "tts": {"status": {"ready": True, "voice": "virtual", "simulated": True}},
    "stt": {"status": {"ready": True, "language": "en-US", "simulated": True}},
    "agent": {"status": {"ready": True, "provider": "virtual", "simulated": True}},
}

class State:
    def __init__(self, root: Path):
        self.root = root
        self.lock = threading.Lock()
        self.faults: set[str] = set()
        self.data = json.loads(json.dumps(SERVICES))

    def response(self, service: str, cmd: str, args: dict) -> dict:
        with self.lock:
            fault_file = self.root / "state" / "virtual.json"
            try:
                saved = json.loads(fault_file.read_text())
                self.faults = set(saved.get("faults", []))
            except (FileNotFoundError, ValueError, TypeError):
                pass
            if service in self.faults or f"{service}:{cmd}" in self.faults:
                return {"ok": False, "error": f"virtual fault injected: {service}:{cmd}"}
            if cmd == "status":
                return {"ok": True, "data": self.data.get(service, {}).get("status", {})}
            if service == "network" and cmd == "scan":
                return {"ok": True, "data": self.data[service]["scan"]}
            if service == "network" and cmd in ("connect", "disconnect"):
                status = self.data[service]["status"]
                if cmd == "disconnect":
                    status.update(state="disconnected", connectivity="disconnected", gateway_reachable=False, internet=False)
                else:
                    status.update(state="connected", connectivity="healthy", gateway_reachable=True, internet=True,
                                  ssid=args.get("ssid", "VirtualEchoNet"))
                return {"ok": True, "data": status}
            if service == "audio" and cmd in ("set_volume", "set_gain", "set_mute"):
                status = self.data[service]["status"]
                key = {"set_volume": "volume", "set_gain": "microphone_gain", "set_mute": "muted"}[cmd]
                value = args.get({"volume": "volume", "microphone_gain": "gain", "muted": "muted"}[key])
                status[key] = value
                return {"ok": True, "data": status}
            if service == "led" and cmd in ("set_colour", "set_brightness", "set_visualizer_enabled"):
                status = self.data[service]["status"]
                if cmd == "set_colour": status["colour"] = {k: args.get(k, 0) for k in ("r", "g", "b")}
                elif cmd == "set_brightness": status["brightness"] = args.get("brightness", 0)
                else: status["visualizer_enabled"] = bool(args.get("enabled", False))
                return {"ok": True, "data": status}
            if service == "wakeword" and cmd in ("set_word", "set_sensitivity", "test"):
                status = self.data[service]["status"]
                if cmd == "set_word": status["wake_word"] = args.get("word", status["wake_word"])
                elif cmd == "set_sensitivity": status["sensitivity"] = args.get("sensitivity", status["sensitivity"])
                else: status["detected_count"] += 1
                return {"ok": True, "data": status}
            if service in ("bluetooth", "airplay", "tts", "stt", "agent", "mic"):
                return {"ok": True, "data": self.data[service].get("status", {})}
            return {"ok": True, "data": {}}

class AdapterServer(threading.Thread):
    daemon = True
    def __init__(self, service: str, path: Path, state: State):
        super().__init__(name=service)
        self.service, self.path, self.state = service, path, state
        self.stop_event = threading.Event()
        self.sock: socket.socket | None = None

    def run(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        try: self.path.unlink()
        except FileNotFoundError: pass
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(str(self.path)); self.sock.listen(16); self.sock.settimeout(0.25)
        os.chmod(self.path, 0o660)
        while not self.stop_event.is_set():
            try: client, _ = self.sock.accept()
            except socket.timeout: continue
            except OSError: break
            threading.Thread(target=self.client, args=(client,), daemon=True).start()

    def client(self, client: socket.socket) -> None:
        with client:
            buf = b""
            while b"\n" not in buf and len(buf) < 4096:
                chunk = client.recv(4096)
                if not chunk: return
                buf += chunk
            try:
                request = json.loads(buf.split(b"\n", 1)[0])
                result = self.state.response(self.service, request.get("cmd", ""), request.get("args") or {})
                reply = {"v": 1, "id": request.get("id", 0), **result}
            except (ValueError, TypeError):
                reply = {"v": 1, "id": 0, "ok": False, "error": "invalid virtual request"}
            client.sendall((json.dumps(reply, separators=(",", ":")) + "\n").encode())

    def stop(self) -> None:
        self.stop_event.set()
        if self.sock:
            self.sock.close()
        try: self.path.unlink()
        except FileNotFoundError: pass


def write_state(root: Path, state: State) -> None:
    (root / "state").mkdir(parents=True, exist_ok=True)
    (root / "state" / "virtual.json").write_text(json.dumps({"faults": sorted(state.faults)}, indent=2) + "\n")


def run_web(root: Path, host: str, port: int, web: Path) -> subprocess.Popen[bytes]:
    # bwrap makes the production absolute /run and /var/run paths private while
    # leaving the host filesystem read-only to the guest process.
    runtime, data = root / "run" / "libreecho", root / "data"
    for p in (runtime, data / "libreecho" / "config", data / "libreecho" / "userdata"):
        p.mkdir(parents=True, exist_ok=True)
    # The daemon uses the config file's existence as its setup-completed
    # marker. Keep that marker persistent alongside users so a reused virtual
    # root cannot serve the Wi-Fi setup page while requiring account auth.
    config_path = data / "libreecho" / "config" / "web-config.json"
    if not config_path.exists():
        config_path.write_text("{}\n")
        os.chmod(config_path, 0o600)
    cmd = ["bwrap", "--die-with-parent", "--unshare-user-try", "--new-session", "--ro-bind", "/", "/",
           "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp", "--tmpfs", "/run",
           "--dir", "/run/libreecho", "--bind", str(runtime), "/run/libreecho",
           "--bind", str(data / "libreecho"), "/tmp/virtual-data",
           str(web), "--backend", "linux", "--listen", f"{host}:{port}",
           "--web-root", str(web.parent.parent / "web"),
           "--config", "/tmp/virtual-data/config/web-config.json",
           "--users-file", "/tmp/virtual-data/config/users", "--allow-insecure-lan"]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)


def wait_http(host: str, port: int, timeout: float) -> bool:
    probe_host = "127.0.0.1" if host == "0.0.0.0" else "::1" if host == "::" else host
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection((probe_host, port), timeout=0.4) as s:
                s.sendall(b"GET / HTTP/1.0\r\nHost: virtual\r\n\r\n")
                return b"200" in s.recv(256)
        except OSError: time.sleep(0.1)
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("start", "check", "stop", "status", "fault"))
    parser.add_argument("fault_name", nargs="?")
    parser.add_argument("--root", type=Path, default=Path(".virtual-echo"))
    parser.add_argument("--host", default="127.0.0.1", help="listen host; use 0.0.0.0 only for deliberate LAN exposure")
    parser.add_argument("--port", type=int, default=18080)
    parser.add_argument("--timeout", type=float, default=10)
    args = parser.parse_args()
    root = args.root.resolve(); pidfile = root / "web.pid"

    if args.command == "stop":
        if pidfile.exists():
            try: os.kill(int(pidfile.read_text()), signal.SIGTERM)
            except (ValueError, ProcessLookupError): pass
            pidfile.unlink(missing_ok=True)
        for p in (root / "run" / "libreecho").glob("*.sock") if (root / "run" / "libreecho").exists() else (): p.unlink(missing_ok=True)
        print("stopped"); return 0
    if args.command == "status":
        alive = False
        if pidfile.exists():
            try: alive = os.kill(int(pidfile.read_text()), 0) is None
            except (ValueError, OSError): pass
        print(json.dumps({"running": alive, "root": str(root), "port": args.port})); return 0 if alive else 1
    if args.command == "fault":
        state = State(root)
        if args.fault_name: state.faults.add(args.fault_name)
        write_state(root, state); print(json.dumps({"faults": sorted(state.faults)})); return 0

    root.mkdir(parents=True, exist_ok=True)
    state = State(root)
    if args.command == "check":
        ok = wait_http(args.host, args.port, args.timeout)
        print("virtual_echo_ready" if ok else "virtual_echo_not_ready")
        return 0 if ok else 1
    if pidfile.exists():
        print(f"already running: {pidfile}", file=sys.stderr); return 2
    runtime = root / "run" / "libreecho"; runtime.mkdir(parents=True, exist_ok=True)
    servers = [AdapterServer(name, runtime / f"{name if name != 'wakeword' else 'wakeword'}.sock", state)
               for name in SERVICES]
    for server in servers: server.start()
    write_state(root, state)
    web = Path(__file__).resolve().parents[1] / "build" / "libreecho-web"
    if not web.exists():
        print(f"missing {web}; run make build/libreecho-web first", file=sys.stderr)
        for server in servers: server.stop()
        return 2
    proc = run_web(root, args.host, args.port, web)
    pidfile.write_text(str(os.getpid()) + "\n")
    if not wait_http(args.host, args.port, args.timeout):
        err = proc.stderr.read(4096).decode(errors="replace") if proc.stderr else ""
        proc.terminate()
        for server in servers: server.stop()
        pidfile.unlink(missing_ok=True)
        print(err, file=sys.stderr); return 1

    stopping = threading.Event()
    def request_stop(_signum: int, _frame: object) -> None:
        stopping.set()
    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    print(json.dumps({"running": True, "pid": os.getpid(), "port": args.port,
                      "root": str(root), "backend": "linux", "services": sorted(SERVICES)}), flush=True)
    while not stopping.is_set() and proc.poll() is None:
        time.sleep(0.2)
    if proc.poll() is None:
        proc.terminate()
        try: proc.wait(timeout=3)
        except subprocess.TimeoutExpired: proc.kill()
    for server in servers: server.stop()
    pidfile.unlink(missing_ok=True)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
