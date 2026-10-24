#!/bin/sh
# HTTP contract for the Sendspin integration (mock backend, real web daemon):
# the integrations toggle (bit 64, persisted), GET /api/v1/sendspin status
# without any pairing material, and POST /api/v1/sendspin/pairing gated by
# enablement, CSRF and authentication.
#
# Usage: sh tests/test_sendspin_api.sh
# Requires: build/libreecho-web (make build/libreecho-web), jq, curl.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${LIBREECHO_SENDSPIN_TEST_PORT:-18131}
AUTH_PORT=${LIBREECHO_SENDSPIN_TEST_AUTH_PORT:-18132}
URL="http://127.0.0.1:$PORT"
AUTH_URL="http://127.0.0.1:$AUTH_PORT"
BIN="$ROOT/build/libreecho-web"
TMP=$(mktemp -d)
PASS=0
FAIL=0

cleanup() {
    [ -n "${dev_pid:-}" ] && kill "$dev_pid" 2>/dev/null
    [ -n "${auth_pid:-}" ] && kill "$auth_pid" 2>/dev/null
    wait 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

[ -x "$BIN" ] || { echo "missing $BIN; run: make build/libreecho-web" >&2; exit 1; }

ok() { PASS=$((PASS + 1)); }
bad() { FAIL=$((FAIL + 1)); echo "FAIL[$1] $2" >&2; [ -s "$TMP/body" ] && echo "  body: $(cat "$TMP/body")" >&2; }
check() {
    desc=$1; want=$2; shift 2
    got=$(curl -sS -o "$TMP/body" -w '%{http_code}' "$@") || got=000
    if [ "$got" = "$want" ]; then ok; else bad "$desc" "want $want got $got"; fi
}
jcheck() {
    desc=$1; expr=$2; shift 2
    if ! curl -fsS -o "$TMP/body" "$@"; then bad "$desc" "request failed"; return; fi
    if jq -e "$expr" "$TMP/body" >/dev/null 2>&1; then ok; else bad "$desc" "jq: $expr"; fi
}

CFG="$TMP/config.json"
printf '{}' > "$CFG"
"$BIN" --backend mock --config "$CFG" --web-root "$ROOT/web" --listen "127.0.0.1:$PORT" --seed 42 \
    >"$TMP/dev.log" 2>&1 &
dev_pid=$!
i=0
while ! curl -fsS "$URL/api/v1/status" >/dev/null 2>&1; do
    i=$((i + 1)); [ "$i" -lt 150 ] || { echo "web did not start" >&2; cat "$TMP/dev.log" >&2; exit 1; }
    sleep 0.1
done
AUTH="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
JSON='Content-Type: application/json'

echo "== disabled by default =="
jcheck "integration listed, off, installed" \
    '.data.items[] | select(.id=="sendspin") | .enabled==false and .installed==true' "$URL/api/v1/integrations"
jcheck "status disabled" \
    '.data.state=="disabled" and .data.enabled==false and .data.running==false and .data.volume==null and .data.server==null' \
    "$URL/api/v1/sendspin"
check "pairing refused while disabled" 409 -X POST "$URL/api/v1/sendspin/pairing" -H "$AUTH"

echo "== methods, CSRF and validation =="
check "status PUT" 405 -X PUT "$URL/api/v1/sendspin" -H "$AUTH" -H "$JSON" --data '{}'
check "pairing GET" 405 "$URL/api/v1/sendspin/pairing"
check "toggle without CSRF" 403 -X PUT "$URL/api/v1/integrations/sendspin" -H "$JSON" --data '{"enabled":true}'
check "toggle non-boolean" 400 -X PUT "$URL/api/v1/integrations/sendspin" -H "$AUTH" -H "$JSON" --data '{"enabled":"yes"}'
jcheck "still disabled" '.data.enabled==false' "$URL/api/v1/sendspin"

echo "== enable =="
check "toggle on" 200 -X PUT "$URL/api/v1/integrations/sendspin" -H "$AUTH" -H "$JSON" --data '{"enabled":true}'
jcheck "integration on" '.data.items[] | select(.id=="sendspin") | .enabled==true' "$URL/api/v1/integrations"
jcheck "status connected" \
    '.data.state=="connected" and .data.enabled and .data.running and .data.paired and .data.discoverable and (.data.volume|type)=="number" and .data.server=="Music Assistant (mock)"' \
    "$URL/api/v1/sendspin"
if grep -q 'SP:\|token' "$TMP/body"; then bad "status leak" "status carries pairing material"; else ok; fi
jq -e '(.integrations // 0) as $m | ($m / 64 | floor) % 2 == 1' "$CFG" >/dev/null 2>&1 && ok || bad "persisted" "bit 64 not in saved integrations"

echo "== pairing token =="
check "pairing without CSRF" 403 -X POST "$URL/api/v1/sendspin/pairing"
jcheck "pairing token" '.data.pairing_token | startswith("SP:")' -X POST "$URL/api/v1/sendspin/pairing" -H "$AUTH"

echo "== disable =="
check "toggle off" 200 -X PUT "$URL/api/v1/integrations/sendspin" -H "$AUTH" -H "$JSON" --data '{"enabled":false}'
jcheck "status disabled again" '.data.state=="disabled"' "$URL/api/v1/sendspin"

echo "== authentication =="
ACFG="$TMP/auth.json"
printf '{}' > "$ACFG"
printf '%s\n' 'test-token-0123456789abcdef' > "$TMP/token"
chmod 600 "$TMP/token"
"$BIN" --backend mock --config "$ACFG" --web-root "$ROOT/web" --listen "127.0.0.1:$AUTH_PORT" --seed 42 \
    --auth-token-file "$TMP/token" >"$TMP/auth.log" 2>&1 &
auth_pid=$!
i=0
while ! curl -sS -o /dev/null "$AUTH_URL/api/v1/config" 2>/dev/null; do
    i=$((i + 1)); [ "$i" -lt 150 ] || { echo "auth web did not start" >&2; exit 1; }
    sleep 0.1
done
ACSRF="X-LibreEcho-CSRF: $(curl -fsS "$AUTH_URL/api/v1/config" | jq -r '.data.csrf_token')"
check "status needs auth" 401 "$AUTH_URL/api/v1/sendspin"
check "pairing needs auth" 401 -X POST "$AUTH_URL/api/v1/sendspin/pairing" -H "$ACSRF"
check "status with auth" 200 "$AUTH_URL/api/v1/sendspin" -H 'Authorization: Bearer test-token-0123456789abcdef'

echo "sendspin api: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
