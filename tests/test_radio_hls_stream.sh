#!/bin/sh
# End to end HLS + AAC: a local server hands radiod an HLS master, a live media
# playlist that grows, segments cut from the MPEG-TS fixture at packet
# boundaries, and one 302. A reader drains a FIFO standing in for the audio
# bus. Also plays ADTS over a plain Icecast-style response (no ICY headers).
# Offline: nothing here touches the network beyond 127.0.0.1.
set -eu

test_dir=$(mktemp -d)
socket_path="$test_dir/radio.sock"
bus_path="$test_dir/media.pcm"
log_path=./build/test-radio-hls-stream.log
radiod_bin=${LIBREECHO_TEST_RADIOD:-./build/libreecho-radiod}
pid=0
reader=0

cleanup() {
    for p in "$reader" "$pid"; do
        if [ "$p" -gt 1 ]; then
            kill -TERM "$p" 2>/dev/null || true
            i=0
            while kill -0 "$p" 2>/dev/null && [ "$i" -lt 20 ]; do
                sleep 0.1
                i=$((i + 1))
            done
            kill -KILL "$p" 2>/dev/null || true
            wait "$p" 2>/dev/null || true
        fi
    done
    rm -rf "$test_dir" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

mkfifo "$bus_path"
# The reader counts what reaches the bus; it holds the FIFO open for reading.
# radiod reopens the bus on every reconnect, so the reader must outlive each
# writer: a bare cat would exit at the first EOF.
(while :; do cat "$bus_path" >> "$test_dir/pcm.out"; done) &
reader=$!

"$radiod_bin" --socket "$socket_path" --bus "$bus_path" >"$log_path" 2>&1 &
pid=$!

i=0
while [ ! -S "$socket_path" ]; do
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        cat "$log_path"
        exit 1
    fi
    sleep 0.1
done

LIBREECHO_RADIO_TEST_SOCKET="$socket_path" LIBREECHO_RADIO_PCM="$test_dir/pcm.out" \
    timeout 60 python3 - <<'PY'
import json
import os
import socket
import socketserver
import threading
import time

PACKET = 188
ts = open("tests/fixtures/radio/aac-tone.ts", "rb").read()
adts = open("tests/fixtures/radio/aac-tone.adts", "rb").read()
assert len(ts) % PACKET == 0

# Three segments cut at packet boundaries. Each keeps the PAT/PMT at its head
# (the first 3 packets) so a segment is decodable by itself, as a real one is.
per = (len(ts) // PACKET) // 3
head = ts[:3 * PACKET]
segments = []
for n in range(3):
    body = ts[n * per * PACKET:(n + 1) * per * PACKET]
    segments.append(body if n == 0 else head + body)

requests = []
state = {"polls": 0}
lock = threading.Lock()


def media_playlist():
    with lock:
        state["polls"] += 1
        polls = state["polls"]
    # Live: two segments at first, the third appears on a later refresh, then
    # the playlist is closed.
    count = 2 if polls < 2 else 3
    lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:1",
             "#EXT-X-MEDIA-SEQUENCE:100"]
    for n in range(count):
        lines += ["#EXTINF:1.0,", "seg%d.ts" % n]
    if count == 3:
        lines.append("#EXT-X-ENDLIST")
    return ("\n".join(lines) + "\n").encode()


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        line = self.rfile.readline().decode("latin-1").split()
        while self.rfile.readline() not in (b"\r\n", b"\n", b""):
            pass
        if len(line) < 2:
            return
        path = line[1]
        with lock:
            requests.append(path)
        if path.startswith("/live/master.m3u8"):
            body = (b"#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=96000,"
                    b"CODECS=\"mp4a.40.2\"\nmedia.m3u8\n")
            self.reply("application/vnd.apple.mpegurl", body)
        elif path == "/live/media.m3u8":
            # one redirect on the media playlist, to exercise Location
            with lock:
                first = "redirected" not in state
                state["redirected"] = True
            if first:
                self.wfile.write(b"HTTP/1.0 302 Found\r\n"
                                 b"Location: /live/real.m3u8\r\n\r\n")
            else:
                self.reply("application/vnd.apple.mpegurl", media_playlist())
        elif path == "/live/real.m3u8":
            self.reply("application/vnd.apple.mpegurl", media_playlist())
        elif path.startswith("/live/seg") and path.endswith(".ts"):
            n = int(path[len("/live/seg"):-3])
            self.reply("video/mp2t", segments[n])
        elif path == "/icecast.aac":
            self.reply("audio/aac", adts)
        else:
            self.wfile.write(b"HTTP/1.0 404 Not Found\r\n\r\n")

    def reply(self, ctype, body):
        self.wfile.write(b"HTTP/1.0 200 OK\r\nContent-Type: " + ctype.encode() +
                         b"\r\nConnection: close\r\n\r\n" + body)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


server = Server(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
base = "http://127.0.0.1:%d" % server.server_address[1]

sock_path = os.environ["LIBREECHO_RADIO_TEST_SOCKET"]
pcm_path = os.environ["LIBREECHO_RADIO_PCM"]
sequence = 0


def call(command, arguments=None):
    global sequence
    sequence += 1
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(3)
    client.connect(sock_path)
    client.sendall(json.dumps({"v": 1, "id": sequence, "cmd": command,
                               "args": arguments or {}},
                              separators=(",", ":")).encode() + b"\n")
    response = b""
    while not response.endswith(b"\n"):
        response += client.recv(4096)
    client.close()
    parsed = json.loads(response)
    assert parsed["ok"] is True, parsed
    return parsed.get("data", {})


def pcm_bytes():
    try:
        return os.path.getsize(pcm_path)
    except OSError:
        return 0


def wait_for(predicate, what, timeout=15):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError("timed out waiting for %s; requests=%r" % (what, requests))


# ---- HLS ------------------------------------------------------------------
try:
    before = pcm_bytes()
    call("play", {"url": base + "/live/master.m3u8?station=t&bitrate=96000"})
    wait_for(lambda: pcm_bytes() > before, "HLS PCM on the bus")
    assert call("status")["playing"] is True
    # The last segment only appears after a playlist refresh.
    wait_for(lambda: "/live/seg2.ts" in requests, "the third segment")
finally:
    pass

segs = [r for r in requests if r.startswith("/live/seg")]
assert segs == ["/live/seg0.ts", "/live/seg1.ts", "/live/seg2.ts"], segs
assert any(r == "/live/real.m3u8" for r in requests), requests  # redirect followed
wait_for(lambda: pcm_bytes() >= 2 * 44100 * 2 * 2 * 0.9 * 0, "PCM volume")
# Each 2 s tone is ~ 96000 frames @ 48 kHz after resample/pass-through; all
# three slices together must give a substantial amount of audio.
wait_for(lambda: pcm_bytes() > 100000, "a full segment of PCM")
call("stop")
assert call("status")["playing"] is False

# ---- ADTS over an Icecast-style response ------------------------------------
before = pcm_bytes()
call("play", {"url": base + "/icecast.aac"})
try:
    wait_for(lambda: pcm_bytes() > before + 50000, "ADTS PCM from a plain stream")
finally:
    call("stop")

server.shutdown()
print("radiod hls + aac: ok")
PY
