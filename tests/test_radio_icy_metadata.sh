#!/bin/sh
# radiod must ask for ICY metadata, strip the interleaved blocks out of the
# audio and report the stream title it found. The station here is a local
# socket rather than a real broadcaster, so what this proves is the parse and
# the child-to-parent handover -- not that any particular station sends
# metadata, which only a real stream can show.
set -eu

test_dir=$(mktemp -d)
socket_path="$test_dir/radio.sock"
log_path=./build/test-radio-icy.log
radiod_bin=${LIBREECHO_TEST_RADIOD:-./build/libreecho-radiod}
pid=0

# radiod blocks in accept() and installs its handlers with signal(), which
# carries SA_RESTART, so SIGTERM does not interrupt the wait and the daemon
# does not exit. That is a real shutdown bug in radiod and is noted here
# rather than worked around silently; this fixture kills it outright.
cleanup() {
    if [ "$pid" -gt 1 ]; then
        kill -KILL "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$test_dir" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

"$radiod_bin" --socket "$socket_path" --bus /dev/null >"$log_path" 2>&1 &
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

LIBREECHO_RADIO_TEST_SOCKET="$socket_path" python3 - <<'PY'
import json
import os
import socket
import socketserver
import threading
import time

METAINT = 200
STATION = "LibreEcho Test Radio"
FIRST = "Test Artist - First Track"
SECOND = "Test Artist - Second Track"

seen = {"header": None}


def block(text):
    """One ICY metadata block: a length byte counting 16-byte units."""
    if text is None:
        return b"\x00"
    payload = ("StreamTitle='%s';" % text).encode("utf-8")
    padded = payload + b"\x00" * (-len(payload) % 16)
    assert len(padded) // 16 <= 255
    return bytes([len(padded) // 16]) + padded


# A minimal but real MPEG-1 Layer III frame header (128 kbps, 44.1 kHz, mono)
# repeated back to back. The payload is silence-shaped rubbish -- nothing here
# listens -- but the sync words have to be real or minimp3 finds no frame and
# the player gives up before any metadata is read.
FRAME = bytes([0xFF, 0xFB, 0x90, 0xC0]) + b"\x00" * 413


class Station(socketserver.BaseRequestHandler):
    def handle(self):
        request = b""
        while b"\r\n\r\n" not in request:
            chunk = self.request.recv(1024)
            if not chunk:
                return
            request += chunk
        seen["header"] = request.decode("latin-1")
        self.request.sendall(
            b"ICY 200 OK\r\n"
            b"content-type: audio/mpeg\r\n"
            b"icy-name: " + STATION.encode("utf-8") + b"\r\n"
            b"icy-metaint: %d\r\n\r\n" % METAINT
        )
        # The title changes once, so a stale first title cannot pass the test.
        # The first one is held for about a second and a half of blocks: a
        # title that flicks past between two polls would make this flaky
        # rather than strict.
        titles = [FIRST] + [None] * 300 + [SECOND] + [None] * 4000
        audio = FRAME * 64
        try:
            for index, title in enumerate(titles):
                start = (index * METAINT) % len(audio)
                chunk = (audio + audio)[start:start + METAINT]
                self.request.sendall(chunk)
                self.request.sendall(block(title))
                time.sleep(0.005)
        except (BrokenPipeError, ConnectionResetError):
            pass


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


server = Server(("127.0.0.1", 0), Station)
threading.Thread(target=server.serve_forever, daemon=True).start()
url = "http://127.0.0.1:%d/stream" % server.server_address[1]

path = os.environ["LIBREECHO_RADIO_TEST_SOCKET"]
sequence = 0


def call(command, arguments=None, expect_ok=True):
    global sequence
    sequence += 1
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(2)
    client.connect(path)
    client.sendall(
        json.dumps(
            {"v": 1, "id": sequence, "cmd": command, "args": arguments or {}},
            separators=(",", ":"),
        ).encode("ascii")
        + b"\n"
    )
    response = b""
    while not response.endswith(b"\n"):
        response += client.recv(4096)
    client.close()
    parsed = json.loads(response)
    assert parsed["ok"] is expect_ok, parsed
    return parsed.get("data", {})


def wait_for(predicate, what):
    deadline = time.time() + 10
    last = None
    while time.time() < deadline:
        last = call("status")
        if predicate(last):
            return last
        time.sleep(0.1)
    raise AssertionError("timed out waiting for %s; last status %r" % (what, last))


idle = call("status")
assert idle["playing"] is False, idle
assert idle["title"] == "", idle
assert idle["station"] == "", idle

try:
    call("play", {"url": url})
    playing = wait_for(lambda s: s["title"] == FIRST, "the first stream title")
    assert playing["playing"] is True, playing
    assert playing["station"] == STATION, playing
    assert playing["url"] == url, playing

    wait_for(lambda s: s["title"] == SECOND, "the title to change")

    assert seen["header"] is not None
    assert "Icy-MetaData: 1" in seen["header"], seen["header"]
finally:
    call("stop")
stopped = call("status")
assert stopped["playing"] is False, stopped
assert stopped["title"] == "", stopped
assert stopped["station"] == "", stopped

server.shutdown()
print("radiod icy metadata: ok")
PY
