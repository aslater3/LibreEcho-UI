#!/usr/bin/env python3
"""Fake Wyoming TTS server for the voice catalogue test: answers describe with an info event."""
import json, socket, sys

port = int(sys.argv[1])
voices = [{"name": f"en_US-voice{i:02d}-medium", "attribution": {"name": "x", "url": "https://example.invalid"},
           "installed": i % 2 == 0, "description": f"Voice {i} \"quoted\"", "languages": ["en_US"]} for i in range(30)]
voices.append({"name": "en_US-hfc_female-medium", "installed": True, "languages": ["en_US"], "description": "hfc_female"})
data = json.dumps({"asr": [], "tts": [{"name": "piper", "installed": True, "voices": voices}]}).encode()
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", port)); srv.listen(4)
print("ready", flush=True)
while True:
    conn, _ = srv.accept()
    with conn:
        line = b""
        while not line.endswith(b"\n"):
            chunk = conn.recv(1)
            if not chunk:
                break
            line += chunk
        if b'"describe"' in line:
            conn.sendall(json.dumps({"type": "info", "data_length": len(data)}).encode() + b"\n" + data)
