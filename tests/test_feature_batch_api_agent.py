#!/usr/bin/env python3
"""Deterministic agentd stand-in for the feature-batch HTTP contract test.

Answers the voice-history commands with fixed, escaping-heavy payloads so the
API layer's pass-through wrapping and the collection/detail/clear routes can be
exercised over a real HTTP server without hardware. Wire format mirrors
tests/mock_agent_history.py: one JSON request line in, one JSON reply line out.
"""
import json
import os
import socketserver
import sys

SOCKET = sys.argv[1]
os.makedirs(os.path.dirname(SOCKET) or ".", exist_ok=True)
try:
    os.unlink(SOCKET)
except FileNotFoundError:
    pass

ENTRY_ID = 4242
# Quotes, a backslash and a non-ASCII codepoint: proves the value survives JSON
# escaping in both directions.
TRANSCRIPT = 'turn "quoted" \\ backslash caf\u00e9'


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline().decode()
        if not line.strip():
            return
        try:
            req = json.loads(line)
            cmd = req.get("cmd")
            if cmd == "voice_history":
                data = {
                    "history_generation": 7,
                    "capacity": 10,
                    "count": 1,
                    "preview_chars": 64,
                    "turns": [{
                        "id": ENTRY_ID,
                        "timestamp": "2026-01-02T03:04:05+00:00",
                        "status": "completed",
                        "transcript_preview": TRANSCRIPT,
                        "response_preview": "ok",
                        "transcript_truncated": False,
                        "response_truncated": False,
                        "transcript_length": len(TRANSCRIPT),
                        "response_length": 2,
                        "stt_ms": 120,
                        "assistant_ms": 800,
                        "tts_ms": 300,
                        "error": None,
                    }],
                }
            elif cmd == "voice_history_entry":
                args = req.get("args") or {}
                if isinstance(args, str):
                    try:
                        args = json.loads(args or "{}")
                    except ValueError:
                        args = {}
                if args.get("id") != ENTRY_ID:
                    self.reject(req.get("id", 0), "no such voice history entry")
                    return
                data = {
                    "id": ENTRY_ID,
                    "timestamp": "2026-01-02T03:04:05+00:00",
                    "status": "completed",
                    "transcript": TRANSCRIPT,
                    "response": "ok",
                    "transcript_truncated": False,
                    "response_truncated": False,
                    "transcript_length": len(TRANSCRIPT),
                    "response_length": 2,
                    "stt_ms": 120,
                    "assistant_ms": 800,
                    "tts_ms": 300,
                    "error": None,
                    "history_generation": 7,
                }
            elif cmd == "voice_history_clear":
                data = {"cleared": True, "history_generation": 8}
            elif cmd == "history":
                data = {"history_generation": 3, "turns": [{"at_ms": 1, "first_pcm_ms": 3100}]}
            elif cmd == "history_clear":
                data = {"cleared": True}
            else:
                data = {"ready": True}
            self.ok(req.get("id", 0), data)
        except Exception as exc:  # pragma: no cover - fixture diagnostics only
            self.reject(0, str(exc))

    def ok(self, rid, data):
        self.wfile.write((json.dumps({"v": 1, "id": rid, "ok": True, "data": data},
                                     separators=(",", ":")) + "\n").encode())
        self.wfile.flush()

    def reject(self, rid, reason):
        self.wfile.write((json.dumps({"v": 1, "id": rid, "ok": False, "error": reason},
                                     separators=(",", ":")) + "\n").encode())
        self.wfile.flush()


class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True
    allow_reuse_address = True


server = Server(SOCKET, Handler)
try:
    server.serve_forever()
finally:
    server.server_close()
    try:
        os.unlink(SOCKET)
    except FileNotFoundError:
        pass
