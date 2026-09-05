#!/usr/bin/env python3
"""A live buttond must restore mute after the real stub ledd restarts."""
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time


def call(path, command):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(1)
        client.connect(str(path))
        client.sendall(json.dumps({"v": 1, "id": 1, "cmd": command, "args": {}}).encode() + b"\n")
        data = b""
        while not data.endswith(b"\n"):
            part = client.recv(4096)
            assert part, "LED response ended prematurely"
            data += part
        return json.loads(data)["data"]


def wait_for(predicate):
    end = time.monotonic() + 8
    while time.monotonic() < end:
        try:
            if predicate():
                return
        except (OSError, KeyError):
            pass
        time.sleep(0.1)
    raise AssertionError("daemon state did not converge within 8 seconds")


with tempfile.TemporaryDirectory(prefix="le-button-restart-") as directory:
    root = Path(directory)
    audio, led = root / "audio.sock", root / "led.sock"
    done = threading.Event()
    with socket.socket(socket.AF_UNIX) as listener:
        listener.bind(str(audio))
        listener.listen(8)
        listener.settimeout(0.2)

        def serve_audio():
            while not done.is_set():
                try:
                    connection, _ = listener.accept()
                except socket.timeout:
                    continue
                with connection:
                    connection.settimeout(1)
                    request = b""
                    while not request.endswith(b"\n"):
                        part = connection.recv(4096)
                        if not part:
                            break
                        request += part
                    connection.sendall(b'{"v":1,"id":1,"ok":true,"data":{"volume":50,"muted":true}}\n')

        thread = threading.Thread(target=serve_audio)
        thread.start()
        processes = []

        def start_led():
            process = subprocess.Popen(["./build/libreecho-ledd", "--foreground", "--stub", "--socket", str(led)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            processes.append(process)
            wait_for(lambda: led.exists() and call(led, "status") is not None)
            return process

        try:
            first = start_led()
            log = open(root / "button.log", "w")
            button = subprocess.Popen(["./build/libreecho-buttond"], env={**os.environ, "LE_AUDIO_SOCK": str(audio), "LE_LED_SOCK": str(led)}, stdout=log, stderr=log)
            processes.append(button)
            wait_for(lambda: call(led, "status")["pattern_owner"] == "mute")
            first.terminate()
            first.wait(timeout=3)
            led.unlink(missing_ok=True)
            start_led()
            wait_for(lambda: call(led, "status")["pattern_owner"] == "mute")
            assert button.poll() is None
            assert (root / "button.log").read_text().count("mute indicator on") == 1
            log.close()
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                process.wait(timeout=3)
            done.set()
            thread.join(timeout=2)
print("buttond: persistent mute restored after real ledd restart: ok")
