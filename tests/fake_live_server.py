#!/usr/bin/env python3
"""A fake GPT-Live server, speaking the real subscription protocol.

Not a stub: it performs the RFC 6455 handshake with a correct
Sec-WebSocket-Accept, requires client frames to be masked, and exchanges the
frameless-bidi events docs/GPT_LIVE_TRANSPORT.md specifies. That makes it a
usable peer for the device transport when no OpenAI account is available, and
it logs every message it receives so the wire format can be asserted on rather
than trusted.
"""
import base64
import hashlib
import json
import os
import socket
import ssl
import struct
import sys
import threading
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
SAMPLE_RATE = 24000

received = []
received_lock = threading.Lock()


def handshake(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            return None
        data += chunk
    head, _, rest = data.partition(b"\r\n\r\n")
    lines = head.decode("latin1").split("\r\n")
    request_line = lines[0]
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, _, value = line.partition(":")
            headers[key.strip().lower()] = value.strip()
    key = headers.get("sec-websocket-key")
    if not key:
        return None
    accept = base64.b64encode(
        hashlib.sha1((key + GUID).encode()).digest()).decode()
    response = (
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Accept: {accept}\r\n\r\n"
    )
    conn.sendall(response.encode())
    return {"request": request_line, "headers": headers,
            "path": request_line.split(" ")[1] if " " in request_line else "",
            "leftover": rest}


def read_exact(conn, count):
    data = b""
    while len(data) < count:
        chunk = conn.recv(count - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def read_frame(conn, buffer):
    header = read_exact(conn, 2)
    if header is None:
        return None, None
    fin = bool(header[0] & 0x80)
    opcode = header[0] & 0x0F
    masked = bool(header[1] & 0x80)
    length = header[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", read_exact(conn, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read_exact(conn, 8))[0]
    if not masked:
        # RFC 6455: a server MUST close on an unmasked client frame.
        raise AssertionError("client frame was not masked")
    key = read_exact(conn, 4)
    payload = read_exact(conn, length) if length else b""
    if payload is None:
        return None, None
    payload = bytes(b ^ key[i % 4] for i, b in enumerate(payload))
    return opcode, payload


def send_frame(conn, opcode, payload):
    header = bytes([0x80 | opcode])
    length = len(payload)
    if length < 126:
        header += bytes([length])
    elif length <= 0xFFFF:
        header += bytes([126]) + struct.pack(">H", length)
    else:
        header += bytes([127]) + struct.pack(">Q", length)
    conn.sendall(header + payload)


def send_json(conn, payload):
    send_frame(conn, 0x1, json.dumps(payload).encode())


def pcm_tone(count, rate):
    """A deterministic square wave, so the device side can be checked."""
    period = 32
    samples = []
    for i in range(count):
        samples.append(6000 if (i % period) < period // 2 else -6000)
    return struct.pack("<%dh" % count, *samples)


def serve_session(conn, info, log):
    log(f"PATH {info['path']}")
    authorization = info['headers'].get('authorization')
    log("AUTH present=%s length=%d" %
        ("yes" if authorization else "no", len(authorization or "")))
    log(f"ACCOUNT {info['headers'].get('chatgpt-account-id', '(none)')}")

    send_json(conn, {"type": "session.created",
                     "session": {"id": "fake-session-1"}})
    audio_bytes = 0
    delegated = False

    while True:
        opcode, payload = read_frame(conn, info.get("leftover", b""))
        if opcode is None:
            log("CLIENT_CLOSED")
            return
        info["leftover"] = b""
        if opcode == 0x8:
            log("CLOSE_FRAME")
            return
        if opcode == 0x9:
            send_frame(conn, 0xA, payload)
            continue
        if opcode != 0x1:
            continue
        message = json.loads(payload.decode())
        kind = message.get("type")
        with received_lock:
            received.append(message)
        if kind == "session.update":
            session = message.get("session", {})
            tools = [tool.get("name") for tool in session.get("tools", [])]
            log("SESSION_UPDATE model=%s voice=%s tools=%s"
                % (session.get("model"),
                   (session.get("audio") or {}).get("output", {}).get("voice"),
                   ",".join(tools)))
            send_json(conn, {"type": "session.updated",
                             "session": {"id": "fake-session-1"}})
        elif kind == "input_audio_buffer.append":
            raw = base64.b64decode(message["audio"])
            audio_bytes += len(raw)
            log("AUDIO_APPEND bytes=%d total=%d" % (len(raw), audio_bytes))
            if audio_bytes > 6400 and not delegated:
                delegated = True
                send_json(conn, {"type": "input_transcript.added",
                                 "item": {"text": "what time"}})
                send_json(conn, {"type": "turn.done",
                                 "turn": {"role": "user",
                                          "transcript":
                                          "what time is it"}})
                send_json(conn, {
                    "type": "response.function_call_arguments.done",
                    "call_id": "call-1", "name": "device_time",
                    "arguments": "{}"})
                send_json(conn, {"type": "response.done",
                                 "response": {"id": "tool-response"}})
        elif (kind == "conversation.item.create" and
              (message.get("item") or {}).get("type") ==
              "function_call_output"):
            item = message["item"]
            log("FUNCTION_OUTPUT id=%s output=%s"
                % (item.get("call_id"), item.get("output")))
        elif kind == "response.create":
            for _ in range(3):
                send_json(conn, {"type": "response.output_audio.delta",
                                 "delta": base64.b64encode(
                                     pcm_tone(2400, SAMPLE_RATE)).decode()})
            send_json(conn, {"type": "response.done",
                             "response": {"id": "spoken-response"}})


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
    logfile = sys.argv[3] if len(sys.argv) > 3 else "/tmp/fake_live_server.log"
    handle = open(logfile, "a", buffering=1)

    def log(line):
        handle.write("FAKE %s %s\n" % (time.strftime("%H:%M:%S"), line))

    bind_host = os.environ.get("FAKE_LIVE_BIND", "127.0.0.1")
    cert = os.environ.get("FAKE_LIVE_CERT")
    key = os.environ.get("FAKE_LIVE_KEY")
    tls_context = None
    if cert and key:
        tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls_context.load_cert_chain(cert, key)
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((bind_host, port))
    listener.listen(4)
    actual = listener.getsockname()[1]
    print(f"FAKE_LISTENING {actual}", flush=True)
    log(f"LISTENING {actual}")

    deadline = time.time() + seconds
    listener.settimeout(1.0)
    while time.time() < deadline:
        try:
            conn, _ = listener.accept()
        except socket.timeout:
            continue
        try:
            if tls_context:
                conn = tls_context.wrap_socket(conn, server_side=True)
            info = handshake(conn)
            if info:
                serve_session(conn, info, log)
            else:
                log("HANDSHAKE_FAILED")
        except Exception as error:      # noqa: BLE001 - report and continue
            log("SESSION_ERROR %s" % error)
        finally:
            conn.close()
    log("STOPPING")
    print("FAKE_DONE", flush=True)


if __name__ == "__main__":
    main()
