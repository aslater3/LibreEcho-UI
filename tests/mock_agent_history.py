#!/usr/bin/env python3
"""Threaded fake agentd for the assistant history/latency API tests.

The 0.14 feature batch split the old single history ring in two:

  * ``GET /assistant/history`` now proxies the canonical private voice-turn
    collection (``voice_history``), with previews, ids and a generation.
  * ``GET /assistant/latency`` keeps the legacy latency history shape.

This stand-in answers every command the web daemon can send for those routes so
``tests/test_api.sh`` can drive the real HTTP layer without hardware:

  * ``history``              -> legacy latency ring (``history_generation`` + turns)
  * ``history_clear``        -> clears both rings and bumps both generations
  * ``voice_history``        -> voice-turn collection (capacity/count/previews)
  * ``voice_history_entry``  -> one bounded record, or a rejection for a bad id
  * ``voice_history_clear``  -> clears just the canonical ring
  * ``respond``              -> deliberately slow, for the in-flight test

Wire format mirrors src/adapter/adapter_client.c: one JSON request line in, one
JSON reply line out.
"""
import json
import os
import socket
import socketserver
import sys
import time

SOCKET = sys.argv[1]
os.makedirs(os.path.dirname(SOCKET) or ".", exist_ok=True)
try:
    os.unlink(SOCKET)
except FileNotFoundError:
    pass

# One canonical turn, id 4242, escaping-heavy so the API pass-through is
# exercised too. ``at_ms`` drives the browser's newest-first ordering.
VOICE_TURN = {
    "id": 4242,
    "timestamp": "2026-10-01T10:00:00+00:00",
    "status": "completed",
    "transcript_preview": "turn \"quoted\" \\ backslash caf\u00e9",
    "response_preview": "ok",
    "transcript_truncated": False,
    "response_truncated": False,
    "transcript_length": 32,
    "response_length": 2,
    "stt_ms": 120,
    "assistant_ms": 800,
    "tts_ms": 300,
    "error": None,
}

LATENCY_TURN = {
    "at_ms": 1724457600123,
    "stt_audio_ms": 1200,
    "stt_processing_ms": 2800,
    "stt_total_ms": 4000,
    "first_text_ms": 2500,
    "first_announce_ms": 3000,
    "first_pcm_ms": 3100,
    "follow_up": False,
}


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            request = json.loads(self.rfile.readline().decode())
            command = request.get("cmd")
            if command == "voice_history":
                data = {
                    "history_generation": self.server.voice_generation,
                    "capacity": 10,
                    "count": 0 if self.server.voice_cleared else 1,
                    "preview_chars": 64,
                    "turns": [] if self.server.voice_cleared else [VOICE_TURN],
                }
            elif command == "voice_history_entry":
                args = request.get("args") or {}
                if isinstance(args, str):
                    try:
                        args = json.loads(args or "{}")
                    except ValueError:
                        args = {}
                if self.server.voice_cleared or args.get("id") != VOICE_TURN["id"]:
                    self.reject(request.get("id", 0),
                                "voice history entry was not found")
                    return
                data = dict(VOICE_TURN)
                data["transcript"] = VOICE_TURN["transcript_preview"]
                data["response"] = "ok"
                data["history_generation"] = self.server.voice_generation
            elif command == "voice_history_clear":
                self.server.voice_cleared = True
                self.server.voice_generation += 1
                data = {"cleared": True,
                        "history_generation": self.server.voice_generation}
            elif command == "history":
                data = {"history_generation": self.server.generation,
                        "turns": [] if self.server.latency_cleared else [LATENCY_TURN]}
            elif command == "history_clear":
                self.server.latency_cleared = True
                self.server.voice_cleared = True
                self.server.generation += 1
                self.server.voice_generation += 1
                data = {}
            elif command == "respond":
                time.sleep(2.0)
                data = {"queued": True, "text": "test response",
                        "first_text_ms": 25}
            else:
                data = {"ready": True}
            self.ok(request.get("id", 0), data)
        except Exception as exc:  # pragma: no cover - fixture diagnostics only
            self.reject(0, str(exc))

    def ok(self, rid, data):
        self.wfile.write((json.dumps(
            {"v": 1, "id": rid, "ok": True, "data": data},
            separators=(",", ":")) + "\n").encode())
        self.wfile.flush()

    def reject(self, rid, reason):
        self.wfile.write((json.dumps(
            {"v": 1, "id": rid, "ok": False, "error": reason},
            separators=(",", ":")) + "\n").encode())
        self.wfile.flush()


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True
    allow_reuse_address = True
    latency_cleared = False
    voice_cleared = False
    generation = 1
    voice_generation = 1


server = Server(SOCKET, Handler)
server.latency_cleared = False
server.voice_cleared = False
try:
    server.serve_forever()
finally:
    server.server_close()
    try:
        os.unlink(SOCKET)
    except FileNotFoundError:
        pass
