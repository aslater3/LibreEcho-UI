#!/bin/sh
# Persistent local services (#170): GET/PUT /api/v1/local-services against a
# temporary local root and status file. The marker is the only switch; turning
# on creates the layout and clears the boot-loop guard; a symlinked or
# non-directory root is refused; the status reports names and states only.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${LIBREECHO_LOCAL_TEST_PORT:-18096}
URL="http://127.0.0.1:$PORT"
BIN="$ROOT/build/libreecho-web"
TMP=$(mktemp -d)
pid=0
cleanup() { [ "$pid" -gt 0 ] && kill "$pid" 2>/dev/null; wait 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM
[ -x "$BIN" ] || { echo "missing $BIN" >&2; exit 1; }
LOCAL="$TMP/local"
export LIBREECHO_LOCAL_ROOT="$LOCAL" LIBREECHO_LOCAL_STATUS="$TMP/local.status" LIBREECHO_LOCAL_SUPERVISOR="$TMP/libreecho-local"
printf '#!/bin/sh\n' >"$TMP/libreecho-local"; chmod 755 "$TMP/libreecho-local"
"$BIN" --backend mock --config "$TMP/config.json" --web-root "$ROOT/web" --listen "127.0.0.1:$PORT" --seed 42 --dev-controls >"$TMP/server.log" 2>&1 &
pid=$!
i=0; while ! curl -fsS "$URL/api/v1/config" >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 50 ] || { cat "$TMP/server.log"; exit 1; }; sleep 0.1; done
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
put() { curl -sS -o "$TMP/out" -w '%{http_code}' -X PUT "$URL/api/v1/local-services" -H "$CSRF" -H 'Content-Type: application/json' --data "$1"; }

curl -fsS "$URL/api/v1/local-services" | jq -e '.data.available == true and .data.enabled == false and .data.path_safe == true and .data.state == "not-running" and .data.services == []' >/dev/null
[ ! -e "$LOCAL" ]
[ "$(put '{"enabled":true}')" = 200 ]
[ -d "$LOCAL/services" ] && [ -f "$LOCAL/enabled" ]
[ "$(stat -c %a "$LOCAL")" = 700 ] && [ "$(stat -c %a "$LOCAL/enabled")" = 600 ]
jq -e '.data.enabled == true' "$TMP/out" >/dev/null
printf '3\n' >"$LOCAL/.boot-guard"
[ "$(put '{"enabled":false}')" = 200 ]
[ ! -e "$LOCAL/enabled" ] && [ -d "$LOCAL/services" ]
[ "$(put '{"enabled":true}')" = 200 ]
[ ! -e "$LOCAL/.boot-guard" ]   # switching on re-arms the boot-loop guard

cat >"$TMP/local.status" <<'STATUS'
schema=1
enabled=1
state=running
service.dropbear=running
service.startup=exited:0
service.Bad Name=running
service.evil=running"><img
STATUS
curl -fsS "$URL/api/v1/local-services" | jq -e '.data.state == "running" and .data.services == [{"name":"dropbear","state":"running"},{"name":"startup","state":"exited:0"}]' >/dev/null

for body in '{}' '{"enabled":"true"}' '{"enabled":1}' '{"enabled":true,"enabled":false}' '[]'; do
    [ "$(put "$body")" = 400 ]
done
code=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT "$URL/api/v1/local-services" -H 'Content-Type: application/json' --data '{"enabled":false}')
[ "$code" = 403 ]
[ -f "$LOCAL/enabled" ]

# A symlinked root is refused and never followed.
rm -rf "$LOCAL"; mkdir "$TMP/elsewhere"; ln -s "$TMP/elsewhere" "$LOCAL"
[ "$(put '{"enabled":true}')" = 409 ]
[ ! -e "$TMP/elsewhere/enabled" ]
curl -fsS "$URL/api/v1/local-services" | jq -e '.data.path_safe == false and .data.enabled == false' >/dev/null
rm "$LOCAL"; printf 'file' >"$LOCAL"
[ "$(put '{"enabled":true}')" = 409 ]
echo 'local services api: ok'
