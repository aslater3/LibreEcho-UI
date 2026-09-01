#!/bin/sh
set -eu

PORT=${LIBREECHO_E2E_PORT:-18083}
URL=${LIBREECHO_E2E_URL:-http://127.0.0.1:$PORT}
CFG=./build/e2e-config.json
LOG=./build/e2e-server.log

mkdir -p ./build
rm -f "$CFG" "$CFG.bak" "$CFG.tmp" "$CFG.setup-complete" "$LOG"

make build/libreecho-web

./build/libreecho-web \
  --backend mock \
  --config "$CFG" \
  --mock-config ./config/mock-state.json \
  --web-root ./web \
  --listen "127.0.0.1:$PORT" \
  --seed 42 \
  --dev-controls >"$LOG" 2>&1 &
pid=$!

cleanup() {
  if [ "${pid:-0}" -gt 1 ]; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

i=0
while ! curl -fsS "$URL/api/v1/config" >/dev/null 2>&1; do
  i=$((i + 1))
  [ "$i" -lt 50 ] || {
    cat "$LOG" >&2
    exit 1
  }
  sleep 0.1
done

# Put the device into its normal post-setup state. The existing API suite owns
# exhaustive setup validation; browser E2E starts from a deterministic dashboard.
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
setup='{"hostname":"e2e-echo","ssid":"LibreNet-IoT","security":"wpa2","password":"browser-test-secret","volume":52,"wake_word":"LibreEcho","wake_sensitivity":72,"local_only":true,"diagnostic_telemetry":false}'
curl -fsS -X POST "$URL/api/v1/setup" \
  -H "$CSRF" \
  -H 'Content-Type: application/json' \
  --data "$setup" >/dev/null

if ! LIBREECHO_E2E_URL="$URL" node tests/e2e/smoke.cjs; then
  echo '--- libreecho-web E2E server log ---' >&2
  cat "$LOG" >&2
  exit 1
fi

echo 'playwright e2e: ok'
