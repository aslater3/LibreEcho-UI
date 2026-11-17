#!/bin/sh
# Setup wizard time zone (#371): POST /api/v1/setup takes an optional
# timezone/timezone_name pair, rejects bad ones, and older clients keep UTC.
set -eu
cd "$(dirname "$0")/.."
make -s build/libreecho-web >/dev/null
PORT=${LIBREECHO_TZ_SETUP_TEST_PORT:-18371}
URL="http://127.0.0.1:$PORT"
WORK=$(mktemp -d "${TMPDIR:-$PWD/build}/le-tz-setup-XXXXXX")
pid=0
cleanup(){ [ "$pid" -gt 1 ] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT INT TERM
start(){
    rm -f "$WORK/config.json" "$WORK/config.json.bak" "$WORK/config.json.setup-complete"
    ./build/libreecho-web --backend mock --config "$WORK/config.json" \
        --web-root ./web --listen "127.0.0.1:$PORT" --seed 42 --dev-controls >"$WORK/server.log" 2>&1 &
    pid=$!
    i=0
    until curl -fsS "$URL/api/v1/config" >/dev/null 2>&1; do
        i=$((i + 1)); [ "$i" -lt 100 ] || { cat "$WORK/server.log"; exit 1; }; sleep 0.1
    done
    CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
}
stop(){ kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true; pid=0; }
BASE='"hostname":"tz-echo","ssid":"Open","security":"open","password":"","volume":40,"wake_word":"LibreEcho","wake_sensitivity":60,"local_only":true,"diagnostic_telemetry":true,"crash_reports":true'
setup(){ curl -sS -o "$WORK/out.json" -w '%{http_code}' -X POST "$URL/api/v1/setup" -H "$CSRF" -H 'Content-Type: application/json' --data "{$BASE$1}"; }

start
for extra in ',"timezone":"UTC;reboot"' ',"timezone":""' ',"timezone":42' ',"timezone_name":"Europe/London"' \
    ',"timezone":"GMT0BST,M3.5.0/1,M10.5.0","timezone_name":"Europe London"'; do
    [ "$(setup "$extra")" = 400 ] || { echo "setup accepted $extra" >&2; exit 1; }
done
[ "$(setup ',"timezone":"<-03>3","timezone_name":"America/Sao_Paulo"')" = 200 ]
curl -fsS "$URL/api/v1/system/timezone" | jq -e '.data.timezone == "LST3" and .data.name == "America/Sao_Paulo" and .data.source == "manual"' >/dev/null
jq -e '.timezone == "LST3" and .timezone_name == "America/Sao_Paulo"' "$WORK/config.json" >/dev/null
stop

# An older setup client sends no zone: the device keeps UTC.
start
[ "$(setup '')" = 200 ]
curl -fsS "$URL/api/v1/system/timezone" | jq -e '.data.timezone == "UTC" and .data.name == "" and .data.source == "manual"' >/dev/null
stop

# The wizard offers the browser's zone and posts it with setup.
grep -q 'id="setup-timezone"' web/setup.html
grep -q 'src="/js/timezones.js' web/setup.html
grep -q 'Intl.DateTimeFormat().resolvedOptions().timeZone' web/js/timezone-ui.js
grep -q 'timezone_name:name' web/js/setup.js
grep -q 'T.browserName()' web/js/setup.js
echo "timezone setup: ok"
