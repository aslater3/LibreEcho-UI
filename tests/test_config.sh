#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CFG=${LIBREECHO_TEST_CONFIG:-./build/test-suite-config.json}
curl -fsS -X PUT "$URL/api/v1/audio" -H 'X-LibreEcho-CSRF: libreecho-local' -H 'Content-Type: application/json' --data '{"volume":37}' >/dev/null
grep -q '"volume": 37' "$CFG"
! grep -qi 'password' "$(dirname "$CFG")/test-suite-config.json"
exported=$(curl -fsS "$URL/api/v1/config/export" | jq -c '.data')
printf '%s' "$exported" | grep -q '"schema_version":1'
printf '%s' "$exported" | jq -e '.partial == false and .unsupported == []' >/dev/null
! printf '%s' "$exported" | grep -Eqi 'password|auth_token|telemetry_value|logs'
curl -fsS -X PUT "$URL/api/v1/audio" -H 'X-LibreEcho-CSRF: libreecho-local' -H 'Content-Type: application/json' --data '{"volume":10}' >/dev/null
curl -fsS -X POST "$URL/api/v1/config/import" -H 'X-LibreEcho-CSRF: libreecho-local' -H 'Content-Type: application/json' --data "$exported" >/dev/null
curl -fsS "$URL/api/v1/audio" | grep -q '"volume":37'
code=$(curl -sS -o /tmp/le-bad-import.out -w '%{http_code}' -X POST "$URL/api/v1/config/import" -H 'X-LibreEcho-CSRF: libreecho-local' -H 'Content-Type: application/json' --data '{"schema_version":1,"volume":999}')
[ "$code" = 400 ]
mode=$(stat -c '%a' "$CFG" 2>/dev/null || stat -f '%Lp' "$CFG")
[ "$mode" = 600 ]
echo 'config: ok'
