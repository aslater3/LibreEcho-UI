#!/usr/bin/env python3
"""Stand-in for curl in the TIDAL client tests.

Speaks the same contract as the real call in src/tidal.c: options arrive in a
--config file (a pipe), an optional form body on stdin, and the reply is the
body followed by "\\n<http status>". Responses come from tests/fixtures/tidal
and a small state file in $FAKE_TIDAL_DIR, so the device-code flow can answer
"pending" first and "approved" after. Every request is appended to
$FAKE_TIDAL_DIR/requests.log, with argv, so the test can prove no token was
ever on the command line.
"""
import json
import os
import re
import sys
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURES = os.path.join(HERE, "fixtures", "tidal")
STATE_DIR = os.environ["FAKE_TIDAL_DIR"]


def reply(status, body):
    if not isinstance(body, str):
        body = json.dumps(body)
    sys.stdout.write(body + "\n" + str(status))
    sys.exit(0)


def fixture(name):
    with open(os.path.join(FIXTURES, name), encoding="utf-8") as handle:
        return handle.read().strip()


def bump(name):
    path = os.path.join(STATE_DIR, name)
    count = int(open(path).read()) if os.path.exists(path) else 0
    count += 1
    with open(path, "w") as handle:
        handle.write(str(count))
    return count


def media(config):
    """A signed FLAC URL: raw bytes from the requested offset, no status.

    FAKE_TIDAL_MEDIA_BREAK=N makes the first fetch stop after N bytes with
    curl's partial-transfer exit code; FAKE_TIDAL_MEDIA_DELAY slows delivery
    so a test can act while a track is still playing."""
    with open(os.path.join(HERE, "fixtures", "radio", "flac-tone.flac"),
              "rb") as handle:
        data = handle.read()
    match = re.search(r'^range = "(\d+)-"', config, re.M)
    offset = int(match.group(1)) if match else 0
    data = data[offset:]
    fetch = bump("media-fetches")
    limit = int(os.environ.get("FAKE_TIDAL_MEDIA_BREAK", "0"))
    broken = fetch == 1 and limit > 0
    if broken:
        data = data[:limit]
    delay = float(os.environ.get("FAKE_TIDAL_MEDIA_DELAY", "0"))
    out = sys.stdout.buffer
    try:
        for start in range(0, len(data), 2048):
            out.write(data[start:start + 2048])
            out.flush()
            if delay:
                time.sleep(delay)
    except BrokenPipeError:
        sys.exit(23)
    sys.exit(18 if broken else 0)


def main():
    args = sys.argv[1:]
    config_path = args[args.index("--config") + 1]
    with open(config_path, encoding="utf-8") as handle:
        config = handle.read()
    body = sys.stdin.read() if "--data-binary" in args else ""
    url_match = re.search(r'^url = "([^"]*)"', config, re.M)
    if not url_match:
        sys.exit(2)                       # curl: no URL given
    url = url_match.group(1)
    headers = re.findall(r'^header = "([^"]*)"', config, re.M)
    user = re.search(r'^user = "([^"]*)"', config, re.M)
    cacert = re.search(r'^cacert = "([^"]*)"', config, re.M)
    bearer = ""
    for header in headers:
        if header.startswith("Authorization: Bearer "):
            bearer = header[len("Authorization: Bearer "):]
    with open(os.path.join(STATE_DIR, "requests.log"), "a") as log:
        rng = re.search(r'^range = "([^"]*)"', config, re.M)
        log.write(json.dumps({"argv": sys.argv, "url": url, "body": body,
                              "bearer": bearer, "basic": bool(user),
                              "cacert": bool(cacert),
                              "range": rng.group(1) if rng else None})
                  + "\n")
    if os.environ.get("FAKE_TIDAL_OFFLINE"):
        sys.exit(6)                       # curl: could not resolve host
    parsed = urllib.parse.urlparse(url)
    query = urllib.parse.parse_qs(parsed.query)
    if parsed.hostname == "media.example.invalid":
        media(config)
    form = urllib.parse.parse_qs(body)
    path = parsed.path

    if path.endswith("/oauth2/device_authorization"):
        reply(200, {"deviceCode": "device-secret-1", "userCode": "ABCDE",
                    "verificationUri": "link.tidal.com",
                    "verificationUriComplete": "link.tidal.com/ABCDE",
                    "expiresIn": 300, "interval": 2})
    if path.endswith("/oauth2/token"):
        if not user:
            reply(401, {"error": "invalid_client"})
        grant = form.get("grant_type", [""])[0]
        if grant.endswith("device_code"):
            if form.get("device_code", [""])[0] != "device-secret-1":
                reply(400, {"error": "invalid_grant"})
            if os.environ.get("FAKE_TIDAL_DENY"):
                reply(400, {"error": "expired_token"})
            if bump("device-polls") < 2:
                reply(400, {"error": "authorization_pending"})
            reply(200, {"access_token": "access-1", "refresh_token": "refresh-1",
                        "expires_in": int(os.environ.get("FAKE_TIDAL_TTL", "14400")),
                        "token_type": "Bearer", "user_id": 42,
                        "user": {"userId": 42, "countryCode": "GB",
                                 "username": "owner@example.invalid"}})
        if grant == "refresh_token":
            if form.get("refresh_token", [""])[0] != "refresh-1" or \
                    os.environ.get("FAKE_TIDAL_REVOKED"):
                reply(400, {"error": "invalid_grant"})
            count = bump("refreshes")
            reply(200, {"access_token": "access-%d" % (count + 1),
                        "expires_in": 14400, "token_type": "Bearer"})
        reply(400, {"error": "unsupported_grant_type"})

    # Catalogue: needs a live bearer and the session's country.
    if not bearer.startswith("access-") or bearer == "access-1" and \
            os.environ.get("FAKE_TIDAL_FIRST_TOKEN_STALE"):
        reply(401, {"status": 401, "subStatus": 11002,
                    "userMessage": "Token expired"})
    if query.get("countryCode") != ["GB"]:
        reply(400, {"userMessage": "countryCode missing"})
    if path.endswith("/v1/search"):
        if query.get("query") != ["daft punk"]:
            reply(200, {"tracks": {"items": []}, "albums": {"items": []},
                        "playlists": {"items": []}})
        reply(200, fixture("search.json"))
    if re.search(r"/v1/albums/\d+/tracks$", path):
        reply(200, fixture("album_tracks.json"))
    if re.search(r"/v1/playlists/[0-9a-f-]+/tracks$", path):
        reply(200, fixture("playlist_tracks.json"))
    if re.search(r"/v1/tracks/\d+/playbackinfopostpaywall$", path):
        if query.get("audioquality") != ["LOSSLESS"]:
            reply(400, {"userMessage": "quality"})
        reply(200, fixture("playbackinfo.json"))
    if re.search(r"/v1/tracks/\d+$", path):
        reply(200, fixture("track.json"))
    reply(404, {"status": 404, "userMessage": "not found"})


if __name__ == "__main__":
    main()
