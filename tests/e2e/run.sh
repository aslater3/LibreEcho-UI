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
  if [ "${auth_pid:-0}" -gt 1 ]; then
    kill "$auth_pid" 2>/dev/null || true
    wait "$auth_pid" 2>/dev/null || true
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

# A second, credential-protected server lets the browser suite prove that the
# recovery captive landing routes an existing owner to sign-in and never reopens
# the one-time account step. It is a separate daemon so the development-disabled
# server keeps its simple, token-free browser suites.
AUTH_PORT=${LIBREECHO_E2E_AUTH_PORT:-18085}
AUTH_URL="http://127.0.0.1:$AUTH_PORT"
AUTH_CFG=./build/e2e-auth-config.json
AUTH_USERS=./build/e2e-auth-users
LIBREECHO_E2E_AUTH_USER=${LIBREECHO_E2E_AUTH_USER:-recovery-owner}
LIBREECHO_E2E_AUTH_PASSWORD=${LIBREECHO_E2E_AUTH_PASSWORD:-recovery-password-123}
rm -f "$AUTH_CFG" "$AUTH_CFG.bak" "$AUTH_CFG.tmp" "$AUTH_CFG.setup-complete" "$AUTH_USERS"
./tools/create-user.sh "$LIBREECHO_E2E_AUTH_USER" "$LIBREECHO_E2E_AUTH_PASSWORD" >"$AUTH_USERS"
chmod 600 "$AUTH_USERS"
./build/libreecho-web \
  --backend mock \
  --config "$AUTH_CFG" \
  --mock-config ./config/mock-state.json \
  --web-root ./web \
  --listen "127.0.0.1:$AUTH_PORT" \
  --seed 42 \
  --users-file "$AUTH_USERS" >./build/e2e-auth-server.log 2>&1 &
auth_pid=$!
i=0
while [ "$(curl -sS -o /dev/null -w '%{http_code}' "$AUTH_URL/api/v1/config" 2>/dev/null)" != "200" ]; do
  i=$((i + 1))
  [ "$i" -lt 150 ] || {
    cat ./build/e2e-auth-server.log >&2
    exit 1
  }
  sleep 0.1
done
AUTH_CSRF="X-LibreEcho-CSRF: $(curl -fsS "$AUTH_URL/api/v1/config" | jq -r '.data.csrf_token')"
AUTH_TOKEN=$(curl -fsS -X POST "$AUTH_URL/api/v1/auth/login" -H "$AUTH_CSRF" \
  -H 'Content-Type: application/json' \
  --data "{\"username\":\"$LIBREECHO_E2E_AUTH_USER\",\"password\":\"$LIBREECHO_E2E_AUTH_PASSWORD\"}" | jq -r '.data.token')
[ -n "$AUTH_TOKEN" ] && [ "$AUTH_TOKEN" != null ] || { echo 'auth server login failed' >&2; exit 1; }
curl -fsS -X POST "$AUTH_URL/api/v1/setup" -H "Authorization: Bearer $AUTH_TOKEN" -H "$AUTH_CSRF" \
  -H 'Content-Type: application/json' --data "$setup" >/dev/null
export LIBREECHO_E2E_AUTH_URL="$AUTH_URL" LIBREECHO_E2E_AUTH_USER LIBREECHO_E2E_AUTH_PASSWORD

# features-radio.cjs was never wired in here, so its expectations drifted
# unnoticed until nothing it asserted was true any more. It runs after smoke
# because it leaves the simulation feature off, which is how smoke expects to
# find it. feature-batch.cjs runs last: it exercises the LED/audio/recovery
# controls against the shared mock backend and would otherwise disturb the
# earlier suites' expectations.
#
# LIBREECHO_E2E_SUITES narrows the list (the WebKit job runs baby-monitor on its
# own) and LIBREECHO_E2E_BROWSER selects the engine inside baby-monitor.cjs.
SUITES=${LIBREECHO_E2E_SUITES:-"smoke features-radio baby-monitor feature-batch"}
for suite in $SUITES; do
  if ! LIBREECHO_E2E_URL="$URL" node "tests/e2e/$suite.cjs"; then
    echo "--- libreecho-web E2E server log ($suite) ---" >&2
    cat "$LOG" >&2
    exit 1
  fi
done

echo 'playwright e2e: ok'
