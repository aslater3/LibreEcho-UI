#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
bundle=$(curl -fsS -X POST "$URL/api/v1/diagnostics/export" -H "$CSRF" \
    -H 'Content-Type: application/json' --data '{}')
printf '%s' "$bundle" | jq -e \
    '.ok and .data.format == "libreecho-diagnostic-bundle" and
     .data.runtime != null and .data.manifest != null and
     .data.audio.available == false and .data.bluetooth.available == false and
     (.data.network | has("ssid") | not) and (.data.network | has("ip") | not)' >/dev/null
printf '%s\n' 'diagnostic export: missing Linux adapters and malformed-status fixture remain non-fatal'
