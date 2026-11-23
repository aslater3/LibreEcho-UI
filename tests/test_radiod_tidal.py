#!/usr/bin/env python3
"""radiod plays a TIDAL queue end to end against the fake TIDAL curl.

What this proves: "tidal:queue" reads the queue file, asks TIDAL for each
track's LOSSLESS stream, fetches the signed FLAC through curl (never with a
token on the command line), decodes it onto the bus, publishes the title,
resumes a broken transfer from the byte it reached with a freshly signed URL,
and steps through the queue with next/previous. What it cannot prove: CPU
and memory on the device, and real TIDAL behaviour -- those need hardware.

Usage: test_radiod_tidal.py [path/to/libreecho-radiod]
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
RADIOD = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    REPO, "build", "libreecho-radiod")
# 17640 frames at 44.1 kHz resampled to 48 kHz stereo s16.
TRACK_BYTES = 17640 * 48000 // 44100 * 4


def call(sock_path, command, args=None):
    client = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    client.settimeout(5)
    client.connect(sock_path)
    client.sendall(json.dumps({"v": 1, "id": 1, "cmd": command,
                               "args": args or {}}).encode())
    # Read one JSON reply rather than to EOF: a player forked while this
    # connection was open holds a copy of it.
    data = b""
    while True:
        chunk = client.recv(4096)
        if not chunk:
            break
        data += chunk
        try:
            reply = json.loads(data)
        except ValueError:
            continue
        client.close()
        return reply
    client.close()
    return json.loads(data)


def status(sock_path):
    reply = call(sock_path, "status")
    assert reply.get("ok"), reply
    return reply["data"]


def wait_for(predicate, timeout=20.0, what="condition"):
    deadline = time.time() + timeout
    while time.time() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError("timed out waiting for " + what)


def requests(fake_dir):
    path = os.path.join(fake_dir, "requests.log")
    if not os.path.exists(path):
        return []
    with open(path) as handle:
        return [json.loads(line) for line in handle if line.strip()]


def run_case(name, extra_env, check):
    with tempfile.TemporaryDirectory() as tmp:
        fake = os.path.join(tmp, "fake")
        os.mkdir(fake)
        secrets = os.path.join(tmp, "secrets")
        os.mkdir(secrets, 0o700)
        session = os.path.join(secrets, "tidal.json")
        with open(session, "w") as handle:
            json.dump({"schema": 1, "access_token": "access-1",
                       "refresh_token": "refresh-1",
                       "expires_at": int(time.time()) + 3600,
                       "country": "GB", "username": "u"}, handle)
        os.chmod(session, 0o600)
        queue = os.path.join(tmp, "tidal-queue")
        with open(queue, "w") as handle:
            handle.write("1550546\n20115557\n20115558\n")
        bus = os.path.join(tmp, "bus.pcm")
        open(bus, "wb").close()
        sock_path = os.path.join(tmp, "radio.sock")
        env = dict(os.environ)
        env.update({
            "FAKE_TIDAL_DIR": fake,
            "LIBREECHO_TIDAL_CURL": os.path.join(HERE, "fake_tidal_curl.py"),
            "LIBREECHO_TIDAL_CA": os.path.join(HERE, "fixtures", "tidal",
                                               "search.json"),
            "LIBREECHO_TIDAL_SESSION": session,
            "LIBREECHO_TIDAL_LOGIN": os.path.join(tmp, "login.json"),
            "LIBREECHO_TIDAL_QUEUE": queue,
        })
        env.update(extra_env)
        log = open(os.path.join(tmp, "radiod.log"), "w")
        proc = subprocess.Popen([RADIOD, "--socket", sock_path, "--bus", bus],
                                env=env, stdout=log, stderr=log)
        try:
            wait_for(lambda: os.path.exists(sock_path), 5, "radiod socket")
            check(sock_path, bus, fake)
        except Exception:
            log.flush()
            with open(os.path.join(tmp, "radiod.log")) as handle:
                sys.stderr.write(handle.read())
            raise
        finally:
            proc.terminate()
            proc.wait(timeout=5)
            log.close()
    print("radiod tidal: %s: ok" % name)


def check_full_queue(sock_path, bus, fake):
    reply = call(sock_path, "play", {"url": "tidal:queue"})
    assert reply.get("ok"), reply
    wait_for(lambda: status(sock_path)["title"], 10, "a title")
    st = status(sock_path)
    assert st["station"] == "TIDAL", st
    assert st["title"] == "Daft Punk - One More Time", st
    wait_for(lambda: not status(sock_path)["playing"], 30, "queue end")
    size = os.path.getsize(bus)
    assert abs(size - 3 * TRACK_BYTES) < 3 * 4096, (size, 3 * TRACK_BYTES)
    log = requests(fake)
    media = [r for r in log if "media.example.invalid" in r["url"]]
    lookups = [r for r in log if "playbackinfopostpaywall" in r["url"]]
    assert len(media) == 3 and len(lookups) == 3, (len(media), len(lookups))
    assert all(r["cacert"] for r in log)
    assert all(r["range"] == "0-" for r in media), media
    for r in log:
        argv = " ".join(r["argv"])
        assert "access-" not in argv and "token=" not in argv, argv


def check_resume(sock_path, bus, fake):
    reply = call(sock_path, "play", {"url": "tidal:queue#2"})
    assert reply.get("ok"), reply
    wait_for(lambda: not status(sock_path)["playing"], 30, "queue end")
    size = os.path.getsize(bus)
    # One track, cut at 12000 bytes and resumed: the decoder saw one file.
    assert abs(size - TRACK_BYTES) < 4096, (size, TRACK_BYTES)
    media = [r for r in requests(fake) if "media.example.invalid" in r["url"]]
    assert [r["range"] for r in media] == ["0-", "12000-"], media
    lookups = [r for r in requests(fake)
               if "playbackinfopostpaywall" in r["url"]]
    assert len(lookups) == 2, "a resumed transfer re-signs its URL"


def check_next_previous(sock_path, bus, fake):
    assert not call(sock_path, "next").get("ok")      # nothing queued yet
    assert call(sock_path, "play", {"url": "tidal:queue"}).get("ok")
    wait_for(lambda: status(sock_path)["queue_index"] == 0, 10, "index 0")
    assert call(sock_path, "next").get("ok")
    wait_for(lambda: status(sock_path)["queue_index"] == 1, 10, "index 1")
    st = status(sock_path)
    assert st["url"] == "tidal:queue#1" and st["playing"], st
    assert call(sock_path, "previous").get("ok")
    wait_for(lambda: status(sock_path)["queue_index"] == 0, 10, "back to 0")
    assert call(sock_path, "pause").get("ok")
    assert status(sock_path)["paused"]
    assert call(sock_path, "resume").get("ok")
    assert call(sock_path, "stop").get("ok")
    st = status(sock_path)
    assert not st["playing"] and st["queue_index"] == -1, st


def check_signed_out(sock_path, bus, fake):
    # Revoked refresh token, stale access token: the queue ends cleanly.
    assert call(sock_path, "play", {"url": "tidal:queue"}).get("ok")
    wait_for(lambda: not status(sock_path)["playing"], 10, "player exit")
    assert os.path.getsize(bus) == 0
    assert not [r for r in requests(fake)
                if "media.example.invalid" in r["url"]]


def check_bad_urls(sock_path, bus, fake):
    for url in ("tidal:queue#99", "tidal:queue#x", "tidal:other"):
        reply = call(sock_path, "play", {"url": url})
        if reply.get("ok"):
            wait_for(lambda: not status(sock_path)["playing"], 5, "exit")
    assert os.path.getsize(bus) == 0


def main():
    run_case("plays the whole queue", {}, check_full_queue)
    run_case("resumes a broken transfer", {"FAKE_TIDAL_MEDIA_BREAK": "12000"},
             check_resume)
    run_case("next, previous, pause and stop",
             {"FAKE_TIDAL_MEDIA_DELAY": "0.05"}, check_next_previous)
    run_case("a revoked sign-in ends the queue",
             {"FAKE_TIDAL_REVOKED": "1", "FAKE_TIDAL_FIRST_TOKEN_STALE": "1"},
             check_signed_out)
    run_case("rejects bad queue URLs", {}, check_bad_urls)


if __name__ == "__main__":
    main()
