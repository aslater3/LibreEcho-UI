#!/usr/bin/env python3
"""Real radiod + loopback MP3 + private FIFO. No host mixer/USB/media calls.
Usage: python3 tests/test_esphome_radio_controls.py /path/to/radiod
"""
import json
import os
from pathlib import Path
import re
import select
import signal
import socket
import socketserver
import subprocess
import sys
import tempfile
import threading
import time


def wait_for(check, timeout=2):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = check()
        if value:
            return value
        time.sleep(0.01)
    raise AssertionError("bounded fixture wait timed out")


def command(path, name, args=None, ok=True):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(1)
        client.connect(str(path))
        client.sendall(json.dumps({"v": 1, "id": 9, "cmd": name, "args": args or {}}).encode() + b"\n")
        response = b""
        while not response.endswith(b"\n"):
            chunk = client.recv(4096)
            assert chunk, "daemon closed without response"
            response += chunk
            assert len(response) < 4096
    response = json.loads(response)
    assert response["ok"] is ok, response
    return response.get("data")


def children(pid):
    return [int(x) for x in Path(f"/proc/{pid}/task/{pid}/children").read_text().split()]


def drain(fd):
    size = 0
    while select.select([fd], [], [], 0)[0]:
        chunk = os.read(fd, 65536)
        if not chunk:
            break
        size += len(chunk)
    return size


def main():
    binary = Path(sys.argv[1]).resolve()
    fixture = Path(__file__).with_name("radiod_mp3_fixture.h").read_text().split("= {", 1)[1].split("};", 1)[0]
    mp3 = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", fixture))
    assert len(mp3) > 1000
    stopping = threading.Event()
    connections = []

    class Station(socketserver.BaseRequestHandler):
        def handle(self):
            self.request.settimeout(1)
            request = b""
            while b"\r\n\r\n" not in request:
                chunk = self.request.recv(1024)
                if not chunk:
                    return
                request += chunk
                assert len(request) < 8192
            connections.append(request)
            self.request.sendall(b"HTTP/1.0 200 OK\r\nContent-Type: audio/mpeg\r\nicy-name: Fixture Radio\r\n\r\n")
            try:
                while not stopping.is_set():
                    self.request.sendall(mp3)
                    stopping.wait(0.01)
            except (BrokenPipeError, ConnectionResetError, TimeoutError):
                return  # Worker teardown deliberately closes its stream.

    server = socketserver.ThreadingTCPServer(("127.0.0.1", 0), Station)
    thread = threading.Thread(target=server.serve_forever)
    thread.start()
    worker = None
    process = None
    try:
        with tempfile.TemporaryDirectory(prefix="radio-controls-", dir=os.environ["TMPDIR"]) as root:
            root = Path(root)
            bus, path = root / "media.pcm", root / "radio.sock"
            os.mkfifo(bus, 0o600)
            fd = os.open(bus, os.O_RDWR | os.O_NONBLOCK)
            log = open(root / "radio.log", "wb")
            try:
                process = subprocess.Popen([str(binary), "--socket", str(path), "--bus", str(bus)], stdout=log, stderr=log)
                wait_for(path.exists)
                command(path, "pause", ok=False)
                url = f"http://127.0.0.1:{server.server_address[1]}/fixture.mp3"
                command(path, "play", {"url": url})
                wait_for(lambda: drain(fd) > 0)
                worker = wait_for(lambda: children(process.pid))[0]
                before = command(path, "status")
                assert before["playing"] and before["station"] == "Fixture Radio"
                command(path, "pause")
                state = command(path, "status")
                assert state["paused"] and not state["playing"] and state["url"] == url
                assert children(process.pid) == [worker]
                drain(fd)
                assert not select.select([fd], [], [], 0.1)[0], "paused worker is still writing"
                command(path, "resume")
                wait_for(lambda: drain(fd) > 0)
                assert children(process.pid) == [worker] and len(connections) == 1, "resume restarted playback"
                assert command(path, "status")["playing"]
                command(path, "pause")
                start = time.monotonic()
                command(path, "stop")
                assert time.monotonic() - start < 1
                assert not children(process.pid)
                state = command(path, "status")
                assert not state["playing"] and not state["paused"] and not state["url"]
                # Abrupt daemon death while paused must not leave a frozen worker.
                command(path, "play", {"url": url})
                wait_for(lambda: drain(fd) > 0)
                worker = wait_for(lambda: children(process.pid))[0]
                command(path, "pause")
                process.kill()
                process.wait(timeout=2)
                wait_for(lambda: not Path(f"/proc/{worker}").exists() or
                         Path(f"/proc/{worker}/stat").read_text().split(")", 1)[1].split()[0] == "Z")
                print("real radiod: MP3/FIFO pause, same worker+connection resume, paused stop, parent-death cleanup: ok")
            finally:
                if process and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=2)
                os.close(fd)
                log.close()
    finally:
        stopping.set()
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
        assert not thread.is_alive()
        print("fixture cleanup: daemon exited, workers stopped, FIFO/socket removed, server joined")


if __name__ == "__main__":
    main()
