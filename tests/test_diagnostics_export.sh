#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"

code=$(curl -sS -o /tmp/le-diagnostic-export-no-csrf.out -w '%{http_code}' \
    -X POST "$URL/api/v1/diagnostics/export" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 403 ]
code=$(curl -sS -o /tmp/le-diagnostic-export-get.out -w '%{http_code}' \
    "$URL/api/v1/diagnostics/export")
[ "$code" = 405 ]

bundle=$(curl -fsS -X POST "$URL/api/v1/diagnostics/export" -H "$CSRF" \
    -H 'Content-Type: application/json' --data '{}')
printf '%s' "$bundle" | jq -e \
    '.ok and .data.format == "libreecho-diagnostic-bundle" and
     .data.bounded == true and .data.max_bytes <= 24576 and
     (.data.release_identity.product_version | length) > 0 and
     (.data.release_identity.source_commit | length) > 0 and
     (.data.manifest.redactions | index("wifi_credentials")) != null and
     (.data.manifest.redactions | index("bluetooth_addresses")) != null and
     (.data.network | has("ssid") | not) and
     (.data.network | has("ip") | not) and
     (.data.bluetooth | has("known_devices") | not)' >/dev/null
[ "$(printf '%s' "$bundle" | wc -c)" -le 32768 ]
! printf '%s' "$bundle" | grep -Eiq 'LibreNet-|10:20:30:40:50:60|198\.51\.100\.42|top-secret|DEV-MOCK|password|bearer|/data/|/home/'

if [ -x tools/mockctl.sh ] &&
   [ "$(curl -sS -o /tmp/le-diagnostic-mock-probe.out -w '%{http_code}' -X POST "$URL/api/v1/dev/mock" -H 'Content-Type: application/json' --data '{}' || true)" = 400 ]; then
    LIBREECHO_URL="$URL" tools/mockctl.sh fail-next audio >/dev/null
    degraded=$(curl -fsS -X POST "$URL/api/v1/diagnostics/export" -H "$CSRF" \
        -H 'Content-Type: application/json' --data '{}')
    printf '%s' "$degraded" | jq -e \
        '.ok and .data.audio.available == false and
         .data.release_identity.source_commit != null and
         .data.manifest != null' >/dev/null
fi
printf '%s\n' 'diagnostic export: redaction, auth, bounds, and degraded-subsystem paths ok'
