#!/bin/sh
# Install a real certificate and private CA through the HTTP API, sent as
# ordinary JSON (PEM newlines as \n escapes). tests/test_https_trust_api.sh only
# sends garbage, which is how PEMs that could never be installed shipped. The
# validation lives in src/tls.c, so this builds a separate server linked to
# mbedTLS (ESPHOMED_TLS_LIBS) and skips when that is not available.
set -eu
LIBS=${WEB_TLS_LIBS:-${ESPHOMED_TLS_LIBS:-}}
if [ -z "$LIBS" ] || ! command -v openssl >/dev/null || ! command -v jq >/dev/null; then
    echo "https pem api: skipped (needs mbedTLS libs, openssl and jq)"
    exit 0
fi
W=$(mktemp -d "${TMPDIR:-/tmp}/le-https-pem.XXXXXX")
B=build-tls-pem
pid=0
cleanup(){ [ "$pid" -gt 1 ] && { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }; rm -rf "$W" "$B"; }
trap cleanup EXIT INT TERM
make BUILD="$B" WEB_TLS_LIBS="$LIBS" RADIOD_TLS_LIBS="$LIBS" "$B/libreecho-web" >"$W/make.log" 2>&1 || { tail -30 "$W/make.log"; exit 1; }
PORT=${LIBREECHO_HTTPS_PEM_PORT:-18191}
URL=http://127.0.0.1:$PORT
mkdir -p "$W/cfg"
"./$B/libreecho-web" --backend mock --config "$W/cfg/config.json" --tls-dir "$W/cfg" --mock-config ./config/mock-state.json --web-root ./web --listen "127.0.0.1:$PORT" --seed 42 >"$W/server.log" 2>&1 &
pid=$!
i=0
while ! curl -fsS "$URL/api/v1/status" >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 50 ] || { cat "$W/server.log"; exit 1; }; sleep 0.1; done
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
CERT=$URL/api/v1/https/certificate
CA=$URL/api/v1/https/private-ca
put(){ curl -sS -o "$W/out" -w '%{http_code}' -X PUT "$1" -H "$CSRF" -H 'Content-Type: application/json' --data-binary @"$2"; }
chk(){ u=$1; shift; curl -fsS "$u" | jq -e "$@" >/dev/null; }
expect(){ [ "$1" = "$2" ] || { echo "https pem api: $3: got HTTP $1, want $2"; cat "$W/out"; exit 1; }; }

chk "$CERT" '.data.tls_available == true and .data.custom == false'
orig_fp=$(curl -fsS "$CERT" | jq -r '.data.https_fingerprint')

cd "$W"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout key.pem -out cert.pem -subj /CN=le-test -days 30 -addext basicConstraints=critical,CA:FALSE 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout other-key.pem -out other.pem -subj /CN=other -days 30 2>/dev/null
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout ca-key.pem -out ca.pem -subj /CN=le-test-ca -days 30 -addext basicConstraints=critical,CA:TRUE 2>/dev/null
have_expired=0
if openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -keyout old-key.pem -out old.pem -subj /CN=old -not_before 20200101000000Z -not_after 20200102000000Z 2>/dev/null; then have_expired=1; fi
cd - >/dev/null
jq -Rs --rawfile k "$W/key.pem" '{certificate_pem:., private_key_pem:$k}' "$W/cert.pem" >"$W/good.json"
grep -q '\\n' "$W/good.json"   # the body really carries \n escapes
jq -Rs --rawfile k "$W/other-key.pem" '{certificate_pem:., private_key_pem:$k}' "$W/cert.pem" >"$W/mismatch.json"
jq -Rs '{ca_pem:.}' "$W/ca.pem" >"$W/ca.json"
jq -Rs '{ca_pem:.}' "$W/cert.pem" >"$W/leaf-ca.json"
# CRLF line endings are legal JSON escapes too.
jq -Rs '{ca_pem:(gsub("\n";"\r\n"))}' "$W/ca.pem" >"$W/ca-crlf.json"
printf '{"ca_pem":"-----BEGIN CERTIFICATE-----\\nnotbase64!!\\n-----END CERTIFICATE-----\\n"}' >"$W/garbage-ca.json"

# Certificate: bad inputs first, then the real one.
expect "$(put "$CERT" "$W/mismatch.json")" 400 "mismatched pair"
if [ "$have_expired" = 1 ]; then
    jq -Rs --rawfile k "$W/old-key.pem" '{certificate_pem:., private_key_pem:$k}' "$W/old.pem" >"$W/expired.json"
    expect "$(put "$CERT" "$W/expired.json")" 400 "expired certificate"
fi
chk "$CERT" '.data.custom == false'
expect "$(put "$CERT" "$W/good.json")" 200 "valid certificate"
jq -e '.data.custom == true' "$W/out" >/dev/null
chk "$CERT" --arg fp "$orig_fp" '.data.custom == true and .data.https_fingerprint != $fp'
cmp -s "$W/cfg/https-cert.pem" "$W/cert.pem"   # decoded bytes are exactly the PEM
cmp -s "$W/cfg/https-key.pem" "$W/key.pem"

# Private CA.
expect "$(put "$CA" "$W/garbage-ca.json")" 400 "garbage CA"
expect "$(put "$CA" "$W/leaf-ca.json")" 400 "leaf certificate as CA"
chk "$CA" '.data.configured == false'
expect "$(put "$CA" "$W/ca.json")" 200 "valid CA"
jq -e '.data.configured == true and .data.certificates == 1' "$W/out" >/dev/null
chk "$CA" '.data.configured == true'
cmp -s "$W/cfg/private-ca.pem" "$W/ca.pem"
expect "$(put "$CA" "$W/ca-crlf.json")" 200 "valid CA with CRLF escapes"

# DELETE restores the defaults.
code=$(curl -sS -o "$W/out" -w '%{http_code}' -X DELETE "$CA" -H "$CSRF"); expect "$code" 200 "delete CA"
chk "$CA" '.data.configured == false'
code=$(curl -sS -o "$W/out" -w '%{http_code}' -X DELETE "$CERT" -H "$CSRF"); expect "$code" 200 "delete certificate"
chk "$CERT" '.data.custom == false'
echo "https pem api: ok"
