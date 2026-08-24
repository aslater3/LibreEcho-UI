#!/usr/bin/env python3
"""Threaded fake agentd for the successful assistant-history API test."""
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

class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        try:
            request = json.loads(self.rfile.readline().decode())
            command = request.get("cmd")
            if command == "history":
                data = {} if self.server.cleared else {"turns": [{
                    "at_ms": 1724457600123,
                    "stt_audio_ms": 1200,
                    "stt_processing_ms": 2800,
                    "stt_total_ms": 4000,
                    "first_text_ms": 2500,
                    "first_announce_ms": 3000,
                    "first_pcm_ms": 3100,
                    "follow_up": False,
                }]}
                if self.server.cleared:
                    data = {"turns": []}
            elif command == "history_clear":
                self.server.cleared = True
                data = {}
            elif command == "respond":
                time.sleep(2.0)
                data = {"queued": True, "text": "test response", "first_text_ms": 25}
            else:
                data = {"ready": True}
            response = {"v": 1, "id": request.get("id", 0), "ok": True, "data": data}
            self.wfile.write((json.dumps(response, separators=(",", ":")) + "\n").encode())
            self.wfile.flush()
        except Exception as exc:
            response = {"v": 1, "id": 0, "ok": False, "error": str(exc)}
            self.wfile.write((json.dumps(response) + "\n").encode())
            self.wfile.flush()

class Server(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True
    allow_reuse_address = True
    cleared = False

server = Server(SOCKET, Handler)
server.cleared = False
try:
    server.serve_forever()
finally:
    server.server_close()
    try:
        os.unlink(SOCKET)
    except FileNotFoundError:
        pass
