#!/usr/bin/env python3
"""Hermetic stand-in for the Wyoming STT server.

Speaks the same wire protocol as src/adapter/wyoming_protocol.c: a JSON
header line, then data_length bytes of JSON, then payload_length bytes of
binary.  It records how much audio sttd actually streamed and its RMS, then
returns a canned transcript.

The recording matters: without it a hermetic run would pass even if the
capture path delivered silence, which is exactly the failure mode this
harness exists to catch.  The harness asserts on the recorded level, not
just on the transcript coming back.
"""
import argparse, json, math, socket, socketserver, struct, sys, threading

STATE = {"samples": 0, "sum_squares": 0.0, "turns": 0}
LOCK = threading.Lock()


def read_event(f):
    line = f.readline()
    if not line:
        return None, b""
    header = json.loads(line)
    dn = header.get("data_length") or 0
    if dn:
        header["data"] = json.loads(f.read(dn).decode("utf-8"))
    pn = header.get("payload_length") or 0
    return header, (f.read(pn) if pn else b"")


def send_event(f, etype, data=None, payload=b""):
    header = {"type": etype}
    blob = b""
    if data is not None:
        blob = json.dumps(data).encode()
        header["data_length"] = len(blob)
    if payload:
        header["payload_length"] = len(payload)
    f.write((json.dumps(header) + "\n").encode())
    if blob:
        f.write(blob)
    if payload:
        f.write(payload)
    f.flush()


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        samples = 0
        sum_squares = 0.0
        while True:
            try:
                header, payload = read_event(self.rfile)
            except Exception:
                return
            if header is None:
                return
            t = header.get("type")
            if t == "describe":
                send_event(self.wfile, "info",
                           {"asr": [{"name": "harness-fake",
                                     "attribution": {"name": "", "url": ""},
                                     "installed": True,
                                     "models": [{"name": "fake",
                                                 "languages": ["en"],
                                                 "attribution": {"name": "", "url": ""},
                                                 "installed": True}]}]})
            elif t == "audio-chunk" and payload:
                n = len(payload) // 2
                if n:
                    values = struct.unpack("<%dh" % n, payload[:n * 2])
                    samples += n
                    sum_squares += sum(float(v) * v for v in values)
                    # Keep the turn audio when asked. Whether the command
                    # survived a long pause is a question about what actually
                    # reached the recogniser, and only the audio answers it.
                    if self.server.turn_audio:
                        with open(self.server.turn_audio, "ab") as fh:
                            fh.write(payload[:n * 2])
            elif t == "audio-stop":
                rms = math.sqrt(sum_squares / samples) if samples else 0.0
                with LOCK:
                    STATE["samples"] += samples
                    STATE["sum_squares"] += sum_squares
                    STATE["turns"] += 1
                    turns = STATE["turns"]
                print("fake-stt: turn %d received %d samples (%.0f ms) rms=%.0f"
                      % (turns, samples, samples / 16.0, rms), flush=True)
                if self.server.record:
                    with open(self.server.record, "a") as fh:
                        fh.write("%d %d %.1f\n" % (turns, samples, rms))
                send_event(self.wfile, "transcript",
                           {"text": self.server.transcript})
                return


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=31310)
    ap.add_argument("--transcript", default="what time is it")
    ap.add_argument("--record")
    ap.add_argument("--turn-audio")
    args = ap.parse_args()
    server = Server(("127.0.0.1", args.port), Handler)
    server.transcript = args.transcript
    server.record = args.record
    server.turn_audio = args.turn_audio
    print("fake-stt: listening on 127.0.0.1:%d" % args.port, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
