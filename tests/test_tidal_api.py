#!/usr/bin/env python3
"""The TIDAL web routes, end to end through the real HTTP server.

libreecho-web runs with the mock backend and the fake TIDAL curl, so this
proves the owner-facing contract -- sign in with a device code, search, play
an album, skip, sign out -- without a TIDAL account. It also proves that no
route ever returns a token, and that the slow routes run off the event loop.
"""
import json
import os
from pathlib import Path
import socket
import stat
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
HERE = ROOT / "tests"


def main():
    with tempfile.TemporaryDirectory(prefix="le-tidal-api-") as tmp:
        tmp = Path(tmp)
        fake = tmp / "fake"
        fake.mkdir()
        secrets = tmp / "secrets"
        secrets.mkdir(mode=0o700)
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        config = tmp / "config.json"
        config.write_text("{}")
        env = dict(os.environ,
                   FAKE_TIDAL_DIR=str(fake),
                   FAKE_TIDAL_MEDIA_DELAY="0.05",
                   LIBREECHO_TIDAL_CURL=str(HERE / "fake_tidal_curl.py"),
                   LIBREECHO_TIDAL_CA=str(HERE / "fixtures/tidal/search.json"),
                   LIBREECHO_TIDAL_SESSION=str(secrets / "tidal.json"),
                   LIBREECHO_TIDAL_LOGIN=str(tmp / "tidal-login.json"),
                   LIBREECHO_TIDAL_QUEUE=str(tmp / "tidal-queue"))
        log_path = tmp / "web.log"
        with log_path.open("wb") as log:
            web = subprocess.Popen(
                [str(ROOT / "build/libreecho-web"), "--backend", "mock",
                 "--config", str(config),
                 "--mock-config", str(ROOT / "config/mock-state.json"),
                 "--web-root", str(ROOT / "web"),
                 "--listen", f"127.0.0.1:{port}", "--dev-controls"],
                env=env, stdout=log, stderr=log)
            base = f"http://127.0.0.1:{port}"
            csrf = None

            def call(route, body=None, method=None):
                data = json.dumps(body).encode() if body is not None else None
                headers = {"Content-Type": "application/json"}
                if csrf:
                    headers["X-LibreEcho-CSRF"] = csrf
                req = urllib.request.Request(
                    base + route, data=data, headers=headers,
                    method=method or ("POST" if body is not None else "GET"))
                try:
                    with urllib.request.urlopen(req, timeout=10) as reply:
                        return reply.status, json.loads(reply.read())
                except urllib.error.HTTPError as error:
                    return error.code, json.loads(error.read())

            try:
                for _ in range(200):
                    try:
                        status, body = call("/api/v1/config")
                        if status == 200:
                            break
                    except OSError:
                        pass
                    time.sleep(0.02)
                else:
                    raise AssertionError("web did not start")
                csrf = body["data"]["csrf_token"]
                every_reply = []

                def ok(route, body=None, expect=200):
                    status, reply = call(route, body)
                    every_reply.append(json.dumps(reply))
                    assert status == expect, (route, status, reply)
                    return reply["data"]

                tidal = "/api/v1/tidal"
                st = ok(tidal)
                assert st["available"] and not st["signed_in"], st
                assert st["login"] is None and st["quality"] == "LOSSLESS"
                ok(tidal + "/search", {"query": "daft punk"}, 401)

                # Device-code sign-in: link + code, pending, then approved.
                st = ok(tidal + "/login", {})
                login = st["login"]
                assert login["link"].startswith("https://link.tidal.com/"), st
                assert login["user_code"] and login["interval"] > 0, st
                assert login["expires_at"] > time.time(), st
                st = ok(tidal + "/login/poll", {})
                assert not st["signed_in"] and st["login"], st
                st = ok(tidal + "/login/poll", {})
                assert st["signed_in"] and st["login"] is None, st
                assert st["username"] and st["country"] == "GB", st
                session = secrets / "tidal.json"
                assert stat.S_IMODE(session.stat().st_mode) == 0o600
                print("tidal api: device-code sign-in: ok")

                # Search returns all three kinds.
                found = ok(tidal + "/search", {"query": "daft punk"})
                assert found["tracks"] and found["albums"], found
                assert found["tracks"][0]["kind"] == "track"
                album = found["albums"][0]
                assert album["kind"] == "album" and album["id"], album
                ok(tidal + "/search", {"query": ""}, 400)
                print("tidal api: search: ok")

                # Play an album from its second track; skip either way.
                played = ok(tidal + "/play",
                            {"kind": "album", "id": album["id"], "start": 1})
                assert played["queued"] >= 2 and played["start"] == 1, played
                queue = (tmp / "tidal-queue").read_text().split()
                assert len(queue) == played["queued"], queue
                assert ok(tidal + "/next", {})["queue_index"] == 2
                assert ok(tidal + "/previous", {})["queue_index"] == 1
                ok(tidal + "/play", {"kind": "album", "id": "1/../2"}, 400)
                ok(tidal + "/play", {"kind": "artist", "id": "1"}, 400)
                ok(tidal + "/play",
                   {"kind": "album", "id": album["id"], "start": 999}, 400)
                playback = ok("/api/v1/playback")
                assert playback["transport"]["stop"], playback
                print("tidal api: play, next, previous: ok")

                # Sign out stops TIDAL playback and forgets the tokens.
                st = ok(tidal + "/logout", {})
                assert not st["signed_in"] and not session.exists(), st
                assert ok(tidal + "/next", {}, 409) is None
                method_check(call, tidal + "/login")
                print("tidal api: sign out: ok")

                # No reply ever carried a token.
                for text in every_reply:
                    for secret in ("access-", "refresh-", "device-secret"):
                        assert secret not in text, (secret, text)
                print("tidal api: no tokens in replies: ok")
            except Exception:
                web.terminate()
                web.wait(timeout=5)
                print(log_path.read_text())
                raise
            web.terminate()
            web.wait(timeout=5)


def method_check(call, route):
    """GET on a POST-only route is refused, not run."""
    status, _ = call(route)
    assert status == 405, status


if __name__ == "__main__":
    main()
