#!/usr/bin/env python3
"""A stand-in waked: the two subscriptions lived needs, and a wake event.
Streams sample-indexed post-AEC PCM frames on `stream_audio` and publishes one
`wake_detected` after a delay, which is exactly what the real daemon does.
"""
import json
import os
import socket
import struct
import sys
import threading
import time
MAGIC = 0x3153564C
HEADER = 24
FRAME_SAMPLES = 160          # 10 ms at 16 kHz
def frame(first_sample, samples):
    payload = struct.pack("<%dh" % len(samples), *samples)
    return struct.pack("<IHHQI I", MAGIC, 1, 0, first_sample,
                       len(samples), 0)[:HEADER] + payload
def read_line(conn):
    data = b""
    while not data.endswith(b"\n"):
        chunk = conn.recv(1)
        if not chunk:
            return None
        data += chunk
    return data.decode()
def main():
    path = sys.argv[1]
    wake_after = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
    if os.path.exists(path):
        os.unlink(path)
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    listener.bind(path)
    listener.listen(4)
    print("WAKED_LISTENING", flush=True)
    wake_conn = listener.accept()[0]
    request = read_line(wake_conn)
    print("WAKED_SUBSCRIBE %s" % request, flush=True)
    wake_conn.sendall(b'{"v":1,"id":1,"ok":true,"data":{"subscribed":true}}\n')

    audio_conn = listener.accept()[0]
    request = read_line(audio_conn)
    print("WAKED_STREAM %s" % request, flush=True)
    audio_conn.sendall(
        b'{"v":1,"id":1,"ok":true,"data":{"streaming":true,'
        b'"format":"pcm_s16_le","sample_rate":16000,"channels":1,'
        b'"frame_header_bytes":24,"sample_indexed":true}}\n')

    # Enough audio to fill the preroll window before the wake fires.
    sample = 0
    deadline = time.time() + wake_after
    while time.time() < deadline:
        samples = [120 if (i % 2) else -120 for i in range(FRAME_SAMPLES)]
        audio_conn.sendall(frame(sample, samples))
        sample += FRAME_SAMPLES
        time.sleep(0.01)

    event = json.dumps({
        "v": 1, "event": "wake_detected",
        "data": {"detection_sample": sample, "score": 0.99,
                 "vad_score": 1.0, "playback_active": False,
                 "model": "alexa_v0.1"}}) + "\n"
    wake_conn.sendall(event.encode())
    print("WAKED_EVENT sample=%d" % sample, flush=True)

    # Keep streaming so the session can run past the wake.
    while True:
        samples = [120 if (i % 2) else -120 for i in range(FRAME_SAMPLES)]
        try:
            audio_conn.sendall(frame(sample, samples))
        except OSError:
            return
        sample += FRAME_SAMPLES
        time.sleep(0.01)
if __name__ == "__main__":
    main()