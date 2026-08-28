#!/bin/sh
# The streaming kernel-log download, against the dev/mock server.
#
# This is the "download the whole log" path: it streams dmesg straight to the
# socket as chunked text/plain, bypassing the fixed JSON response buffer, so
# the size of the log is not capped by the response body.
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}

# --- it is a text/plain attachment, not JSON ------------------------------
hdr=$(curl -sS -m 10 -D - -o /dev/null "$URL/api/v1/diagnostics/kernel.log")
printf '%s' "$hdr" | grep -qi "^HTTP/1.1 200" \
    || { echo "FAIL: not 200"; printf '%s\n' "$hdr" | head -3; exit 1; }
printf '%s' "$hdr" | grep -qi "Content-Type: text/plain" \
    || { echo "FAIL: not text/plain"; exit 1; }
printf '%s' "$hdr" | grep -qi 'Content-Disposition: attachment; filename="libreecho-kernel.log"' \
    || { echo "FAIL: missing attachment disposition"; exit 1; }
printf '%s' "$hdr" | grep -qi "Transfer-Encoding: chunked" \
    || { echo "FAIL: not chunked"; exit 1; }
echo "  streams as a chunked text/plain attachment: ok"

# --- the body is the actual log, delivered whole (not JSON-wrapped) -------
# In a container dmesg may be restricted, so tolerate an empty body but never
# a JSON envelope -- a JSON body here would mean it fell back to the buffered
# path and re-capped the size.
body=$(curl -sS -m 10 "$URL/api/v1/diagnostics/kernel.log")
case "$body" in
    '{'*) echo "FAIL: got a JSON body, not a raw log stream"; exit 1 ;;
esac
echo "  body is raw log text, not a capped JSON envelope: ok"

# --- a write method is refused --------------------------------------------
code=$(curl -sS -m 8 -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/diagnostics/kernel.log")
case "$code" in 2*) echo "FAIL: POST accepted ($code)"; exit 1 ;; esac
echo "  write method refused ($code): ok"

# --- the button targets the streaming endpoint ----------------------------
curl -fsS "$URL/js/app.js" -o /tmp/le-app-stream.js
grep -q "diagnostics/kernel.log" /tmp/le-app-stream.js \
    || { echo "FAIL: button does not call the streaming endpoint"; exit 1; }
echo "  download button uses the streaming endpoint: ok"

echo "kernel log stream: ok"
