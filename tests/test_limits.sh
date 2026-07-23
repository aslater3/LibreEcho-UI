#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
body=$(awk 'BEGIN{printf "{\"x\":\"";for(i=0;i<17000;i++)printf "a";printf "\"}"}')
code=$(curl -sS -o /tmp/le-large.out -w '%{http_code}' -X PUT "$URL/api/v1/audio" -H "$CSRF" -H 'Content-Type: application/json' --data "$body")
[ "$code" = 413 ]
echo 'limits: ok'
