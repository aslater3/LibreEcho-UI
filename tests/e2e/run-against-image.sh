#!/bin/sh
# Browser E2E against the artefact that actually ships.
#
# tests/e2e/run.sh builds libreecho-web for the host and serves web/ straight
# out of the working tree. That misses anything the image build does to them:
# a web asset the build forgets to stage, a daemon that links on the host but
# not for armv7, a version stamp that never made it in. The page can be broken
# in the shipped bundle while the working tree is fine.
#
# So run the real thing: the armv7 binary and the web root taken from the
# staged ramdisk, inside an armv7 container. Playwright's browsers do not run
# on armv7, so the browser stays on the host and drives it over the network --
# which is what LIBREECHO_E2E_URL is for.
#
# Usage: run-against-image.sh [path-to-staged-ramdisk-root]
set -eu

RD=${1:-${LIBREECHO_IMAGE_ROOT:-}}
PORT=${LIBREECHO_E2E_PORT:-18084}
URL="http://127.0.0.1:$PORT"

[ -n "$RD" ] || { echo "usage: $0 <staged-ramdisk-root>" >&2; exit 2; }
WEB="$RD/usr/local/share/libreecho/web"
BIN="$RD/usr/local/sbin/libreecho-web"
[ -x "$BIN" ] || { echo "no libreecho-web at $BIN" >&2; exit 2; }
[ -d "$WEB" ] || { echo "no web bundle at $WEB" >&2; exit 2; }

echo "  binary: $(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
echo "  web:    $(find "$WEB" -type f | wc -l | tr -d ' ') files"

cid=$(docker run -d --platform linux/arm/v7 -p "$PORT:8080" \
  -v "$RD":/image:ro alpine:3.19 sh -c '
    cp -r /image/usr/local/share/libreecho/web /tmp/web
    exec /image/usr/local/sbin/libreecho-web \
      --backend mock --listen 0.0.0.0:8080 \
      --web-root /tmp/web --config /tmp/e2e.json --seed 42 --dev-controls')

cleanup() { docker kill "$cid" >/dev/null 2>&1 || true; docker rm "$cid" >/dev/null 2>&1 || true; }
trap cleanup EXIT INT TERM

i=0
while ! curl -fsS "$URL/api/v1/config" >/dev/null 2>&1; do
  i=$((i + 1))
  [ "$i" -lt 150 ] || { echo '--- container log ---' >&2; docker logs "$cid" >&2; exit 1; }
  sleep 0.2
done
echo "  serving the shipped artefact on $URL"

CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
setup='{"hostname":"e2e-echo","ssid":"LibreNet-IoT","security":"wpa2","password":"browser-test-secret","volume":52,"wake_word":"LibreEcho","wake_sensitivity":72,"local_only":true,"diagnostic_telemetry":false}'
curl -fsS -X POST "$URL/api/v1/setup" -H "$CSRF" \
  -H 'Content-Type: application/json' --data "$setup" >/dev/null

if ! LIBREECHO_E2E_URL="$URL" node tests/e2e/smoke.cjs; then
  echo '--- container log ---' >&2
  docker logs "$cid" >&2
  exit 1
fi
echo 'playwright e2e (shipped image): ok'
