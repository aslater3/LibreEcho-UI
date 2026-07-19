#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CFG=${LIBREECHO_TEST_CONFIG:-./build/test-suite-config.json}
curl -fsS -X PUT "$URL/api/v1/audio" -H 'X-LibreEcho-CSRF: libreecho-local' -H 'Content-Type: application/json' --data '{"volume":37}' >/dev/null
grep -q '"volume": 37' "$CFG"
! grep -qi 'password' "$(dirname "$CFG")/test-suite-config.json"
mode=$(stat -f '%Lp' "$CFG" 2>/dev/null || stat -c '%a' "$CFG")
[ "$mode" = 600 ]
echo 'config: ok'
