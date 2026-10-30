#!/bin/sh
# Time zone setting (#371): PUT/GET /api/v1/system/timezone, persistence,
# Home Assistant source, and the file-authoritative zone while HA is followed.
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CFG=${LIBREECHO_TEST_CONFIG:-./build/test-suite-config.json}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
put(){ curl -sS -o /tmp/le-tz.out -w '%{http_code}' -X PUT "$URL/api/v1/system/timezone" -H "$CSRF" -H 'Content-Type: application/json' --data "$1"; }

# Manual zone with its IANA name; persisted and reported everywhere.
[ "$(put '{"timezone":"GMT0BST,M3.5.0/1,M10.5.0","name":"Europe/London"}')" = 200 ]
jq -e '.ok and .data.timezone == "GMT0BST,M3.5.0/1,M10.5.0" and .data.name == "Europe/London" and .data.source == "manual" and .data.applies == "now"' /tmp/le-tz.out >/dev/null
curl -fsS "$URL/api/v1/system/timezone" | jq -e '.data.timezone == "GMT0BST,M3.5.0/1,M10.5.0" and .data.name == "Europe/London" and .data.source == "manual"' >/dev/null
curl -fsS "$URL/api/v1/system" | jq -e '.data.timezone == "GMT0BST,M3.5.0/1,M10.5.0" and .data.timezone_name == "Europe/London" and .data.timezone_source == "manual"' >/dev/null
jq -e '.timezone == "GMT0BST,M3.5.0/1,M10.5.0" and .timezone_name == "Europe/London" and .timezone_source == "manual"' "$CFG" >/dev/null

# Older clients send only the POSIX string: the stale name is cleared.
[ "$(put '{"timezone":"CST6CDT,M3.2.0,M11.1.0"}')" = 200 ]
jq -e '.data.name == "" and .data.source == "manual"' /tmp/le-tz.out >/dev/null

# Angle-bracket names (tzdata/HA style) are stored as letters the initramfs accepts.
[ "$(put '{"timezone":"<+04>-4","name":"Asia/Dubai"}')" = 200 ]
jq -e '.data.timezone == "LST-4" and .data.name == "Asia/Dubai"' /tmp/le-tz.out >/dev/null

# Invalid input is rejected whole and changes nothing.
for body in '{}' '{"timezone":""}' '{"timezone":"UTC;reboot"}' '{"timezone":"<+04"}' \
    '{"timezone":"UTC0","name":"Europe/London\""}' '{"timezone":"UTC0","name":"Bad Name"}' \
    '{"timezone":"UTC0","source":"cloud"}' '{"timezone":42}' \
    '{"timezone":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"}'; do
    [ "$(put "$body")" = 400 ] || { echo "accepted $body" >&2; exit 1; }
    jq -e '.ok == false and .error.code' /tmp/le-tz.out >/dev/null
done
curl -fsS "$URL/api/v1/system/timezone" | jq -e '.data.timezone == "LST-4" and .data.name == "Asia/Dubai"' >/dev/null
code=$(curl -sS -o /tmp/le-tz.out -w '%{http_code}' -X PUT "$URL/api/v1/system/timezone" -H 'Content-Type: application/json' --data '{"timezone":"UTC"}')
[ "$code" = 403 ]
code=$(curl -sS -o /tmp/le-tz.out -w '%{http_code}' -X POST "$URL/api/v1/system/timezone" -H "$CSRF" -H 'Content-Type: application/json' --data '{"timezone":"UTC"}')
[ "$code" = 405 ]

# Following Home Assistant needs no zone; the current one stays until HA reports.
[ "$(put '{"source":"home_assistant"}')" = 200 ]
jq -e '.data.source == "home_assistant" and .data.timezone == "LST-4"' /tmp/le-tz.out >/dev/null
jq -e '.timezone_source == "home_assistant"' "$CFG" >/dev/null
# esphomed writes HA's zone straight into the file; the web daemon must show
# it and must not write its stale copy back on the next unrelated save.
tmp="$CFG.tz-test"
jq '.timezone = "EST5EDT,M3.2.0,M11.1.0" | .timezone_name = ""' "$CFG" >"$tmp" && mv "$tmp" "$CFG"
curl -fsS "$URL/api/v1/system/timezone" | jq -e '.data.timezone == "EST5EDT,M3.2.0,M11.1.0" and .data.name == "" and .data.source == "home_assistant"' >/dev/null
curl -fsS "$URL/api/v1/system" | jq -e '.data.timezone == "EST5EDT,M3.2.0,M11.1.0"' >/dev/null
jq '.timezone = "MST7MDT,M3.2.0,M11.1.0"' "$CFG" >"$tmp" && mv "$tmp" "$CFG"
code=$(curl -sS -o /tmp/le-tz.out -w '%{http_code}' -X PUT "$URL/api/v1/system/features" -H "$CSRF" -H 'Content-Type: application/json' --data '{"simulation":false}')
[ "$code" = 200 ]
jq -e '.timezone == "MST7MDT,M3.2.0,M11.1.0" and .timezone_source == "home_assistant"' "$CFG" >/dev/null

# Back to manual: the file no longer wins, the chosen zone does.
[ "$(put '{"timezone":"UTC","source":"manual"}')" = 200 ]
jq '.timezone = "JST-9"' "$CFG" >"$tmp" && mv "$tmp" "$CFG"
curl -fsS "$URL/api/v1/system/timezone" | jq -e '.data.timezone == "UTC" and .data.source == "manual" and .data.name == ""' >/dev/null
[ "$(put '{"timezone":"UTC"}')" = 200 ]
jq -e '.timezone == "UTC" and .timezone_source == "manual"' "$CFG" >/dev/null
echo "timezone api: ok"
