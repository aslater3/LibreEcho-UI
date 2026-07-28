#!/usr/bin/env python3
import json
import math
import os
import socket
import struct
import subprocess
import tempfile
import threading
import time


def send_event(connection, event_type, data=None, payload=b""):
    data_bytes = (
        json.dumps(data, separators=(",", ":")).encode()
        if data is not None else b""
    )
    header = {"type": event_type}
    if data_bytes:
        header["data_length"] = len(data_bytes)
    if payload:
        header["payload_length"] = len(payload)
    connection.sendall(
        (json.dumps(header, separators=(",", ":")) + "\n").encode()
        + data_bytes + payload
    )


def read_exact(connection, size):
    result = b""
    while len(result) < size:
        chunk = connection.recv(size - len(result))
        if not chunk:
            raise RuntimeError("unexpected end of Wyoming stream")
        result += chunk
    return result


def read_event(connection):
    line = b""
    while not line.endswith(b"\n"):
        line += read_exact(connection, 1)
    header = json.loads(line)
    data_bytes = read_exact(connection, header.get("data_length", 0))
    payload = read_exact(connection, header.get("payload_length", 0))
    data = json.loads(data_bytes) if data_bytes else {}
    return header["type"], data, payload


class WyomingServer:
    def __init__(self, handler):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.error = None
        self.thread = threading.Thread(
            target=self._run, args=(handler,), daemon=True
        )

    def _run(self, handler):
        try:
            connection, _ = self.listener.accept()
            with connection:
                connection.settimeout(10)
                handler(connection)
        except Exception as error:
            self.error = error
        finally:
            self.listener.close()

    def start(self):
        self.thread.start()

    def finish(self):
        self.thread.join(10)
        if self.thread.is_alive():
            raise RuntimeError("Wyoming test server did not finish")
        if self.error:
            raise self.error


def wait_for_socket(path):
    for _ in range(100):
        if os.path.exists(path):
            return
        time.sleep(0.02)
    raise RuntimeError(f"adapter socket did not appear: {path}")


def adapter_call(path, command, args):
    connection = socket.socket(socket.AF_UNIX)
    connection.settimeout(15)
    connection.connect(path)
    request = {"v": 1, "id": 1, "cmd": command, "args": args}
    connection.sendall(
        (json.dumps(request, separators=(",", ":")) + "\n").encode()
    )
    line = b""
    while not line.endswith(b"\n"):
        line += read_exact(connection, 1)
    return connection, json.loads(line)


def stop_process(process):
    process.terminate()
    try:
        process.wait(3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(3)


def test_stt(directory):
    received_audio = bytearray()

    def whisper(connection):
        event_type, data, _ = read_event(connection)
        assert event_type == "transcribe"
        assert data["name"] == "whisper-small"
        assert read_event(connection)[0] == "audio-start"
        while True:
            event_type, _, payload = read_event(connection)
            if event_type == "audio-chunk":
                received_audio.extend(payload)
            if event_type == "audio-stop":
                break
        send_event(connection, "transcript",
                   {"text": "remote transcription"})

    server = WyomingServer(whisper)
    server.start()
    socket_path = os.path.join(directory, "stt.sock")
    process = subprocess.Popen([
        "./build/libreecho-sttd-wyoming",
        "--socket", socket_path,
        "--model-dir", f"tcp://127.0.0.1:{server.port}",
        "--threads", "1",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        wait_for_socket(socket_path)
        connection, response = adapter_call(
            socket_path, "recognize_stream", {}
        )
        assert response["ok"] is True
        speech = struct.pack("<h", 2000) * 9600
        short_pause = struct.pack("<h", 0) * 6400
        ending_silence = struct.pack("<h", 0) * 17600
        connection.sendall(speech + short_pause)
        connection.settimeout(0.25)
        try:
            connection.recv(1)
            raise AssertionError(
                "short mid-sentence pause ended recognition")
        except socket.timeout:
            pass
        connection.sendall(speech + ending_silence)
        connection.settimeout(15)
        line = b""
        while not line.endswith(b"\n"):
            line += read_exact(connection, 1)
        event = json.loads(line)
        assert event["event"] == "transcript"
        assert event["data"]["text"] == "remote transcription"
        assert event["data"]["endpoint"] is True
        connection.close()
        server.finish()
        assert len(received_audio) >= len(speech) * 2
    finally:
        stop_process(process)


def test_tts(directory):
    rate = 22050
    text = (
        "The United Kingdom is a country in northwestern Europe, "
        "made up of four constituent nations with a long shared history."
    )
    samples = [
        int(8000 * math.sin(2 * math.pi * 440 * index / rate))
        for index in range(rate // 5)
    ]
    pcm = struct.pack("<" + "h" * len(samples), *samples)

    def piper(connection):
        event_type, data, _ = read_event(connection)
        assert event_type == "synthesize"
        assert data["text"] == text
        assert data["voice"]["name"] == "en_GB-alan-medium"
        audio_format = {"rate": rate, "width": 2, "channels": 1}
        send_event(connection, "audio-start", audio_format)
        send_event(connection, "audio-chunk", audio_format, pcm)
        send_event(connection, "audio-stop", {"timestamp": None})

    server = WyomingServer(piper)
    server.start()
    socket_path = os.path.join(directory, "tts.sock")
    fifo_path = os.path.join(directory, "announcement.pcm")
    os.mkfifo(fifo_path, 0o600)
    fifo = os.open(fifo_path, os.O_RDWR | os.O_NONBLOCK)
    environment = os.environ.copy()
    environment.update({
        "LE_TTS_WYOMING_URI": f"tcp://127.0.0.1:{server.port}",
        "LE_TTS_WYOMING_VOICE": "en_GB-alan-medium",
        "LE_TTS_ANNOUNCEMENT_BUS": fifo_path,
        "LE_TTS_IN_PROCESS": "1",
        "LE_TTS_STREAMING": "1",
    })
    process = subprocess.Popen([
        "./build/libreecho-ttsd-wyoming",
        "--foreground",
        "--socket", socket_path,
        "--model-dir", f"tcp://127.0.0.1:{server.port}",
        "--voice", "en_GB-alan-medium",
    ], env=environment, stdout=subprocess.DEVNULL,
       stderr=subprocess.DEVNULL)
    try:
        wait_for_socket(socket_path)
        connection, response = adapter_call(
            socket_path, "speak",
            {"text": text, "request_id": "test-1"}
        )
        assert response["ok"] is True
        connection.close()
        output = b""
        for _ in range(200):
            try:
                output += os.read(fifo, 16384)
            except BlockingIOError:
                pass
            if len(output) >= 4096:
                break
            time.sleep(0.02)
        server.finish()
        assert len(output) >= 4096
    finally:
        stop_process(process)
        os.close(fifo)


def main():
    with tempfile.TemporaryDirectory(prefix="libreecho-wyoming-") as directory:
        test_stt(directory)
        test_tts(directory)
    print("wyoming engines: remote STT and TTS adapter contracts: ok")


if __name__ == "__main__":
    main()
