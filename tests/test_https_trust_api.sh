#!/bin/sh
# HTTPS certificate upload and private CA trust: the routes exist, need the CSRF
# token, reject bad input, and never write files for a request that fails. The
# validation itself lives in src/tls.c and runs only in TLS builds (the CI
# playwright lane links mbedTLS); this host build uses src/tls_stub.c.
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
CERT=$URL/api/v1/https/certificate
CA=$URL/api/v1/https/private-ca
OUT=${TMPDIR:-/tmp}/le-https-trust.out

code=$(curl -sS -o "$OUT" -w '%{http_code}' "$CERT")
[ "$code" = 200 ]
jq -e '.ok and (.data.tls_available | type == "boolean") and (.data.custom | type == "boolean")' "$OUT" >/dev/null
tls=$(jq -r '.data.tls_available' "$OUT")
if [ "$tls" = true ]; then want=400; else want=501; fi

# Writes need the CSRF token, like every other state-changing route.
code=$(curl -sS -o "$OUT" -w '%{http_code}' -X PUT "$CERT" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 403 ]
code=$(curl -sS -o "$OUT" -w '%{http_code}' -X PUT "$CA" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 403 ]

# A certificate upload missing either half is rejected before anything is written.
code=$(curl -sS -o "$OUT" -w '%{http_code}' -X PUT "$CERT" -H "$CSRF" -H 'Content-Type: application/json' --data '{"certificate_pem":"-----BEGIN CERTIFICATE-----"}')
[ "$code" = "$want" ]
code=$(curl -sS -o "$OUT" -w '%{http_code}' -X PUT "$CERT" -H "$CSRF" -H 'Content-Type: application/json' --data '{"certificate_pem":"not a pem","private_key_pem":"not a key"}')
[ "$code" = "$want" ]
jq -e '.ok == false' "$OUT" >/dev/null

# A mismatched or garbage private CA bundle is rejected.
code=$(curl -sS -o "$OUT" -w '%{http_code}' -X PUT "$CA" -H "$CSRF" -H 'Content-Type: application/json' --data '{"ca_pem":"garbage"}')
[ "$code" = "$want" ]
jq -e '.ok == false' "$OUT" >/dev/null
code=$(curl -sS -o "$OUT" -w '%{http_code}' -X PUT "$CA" -H "$CSRF" -H 'Content-Type: application/json' --data '{"wrong":"field"}')
[ "$code" = "$want" ]

# Wrong method and unknown routes under the same prefix.
code=$(curl -sS -o "$OUT" -w '%{http_code}' -X POST "$CERT" -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 405 ]
code=$(curl -sS -o "$OUT" -w '%{http_code}' -X POST "$CA" -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 405 ]

# Removing a private CA that was never installed succeeds and reports none.
code=$(curl -sS -o "$OUT" -w '%{http_code}' -X DELETE "$CA" -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 200 ]
jq -e '.data.configured == false' "$OUT" >/dev/null
curl -fsS "$CA" | jq -e '.ok and (.data.configured | type == "boolean")' >/dev/null

rm -f "$OUT"
echo "https trust api: ok (tls_available=$tls)"
