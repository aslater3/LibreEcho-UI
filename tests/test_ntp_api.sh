#!/bin/sh
# Time servers (#369): GET/PUT /api/v1/system/ntp write the persistent
# override timed reads (ntp.conf beside the web configuration) with timed's own
# peer rules, and an empty list removes it so the image defaults apply again.
set -eu
W=$(mktemp -d "${TMPDIR:-/tmp}/le-ntp-api.XXXXXX")
pid=0
cleanup(){ [ "$pid" -gt 1 ] && { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }; rm -rf "$W"; }
trap cleanup EXIT INT TERM
PORT=${LIBREECHO_NTP_API_PORT:-18369}
URL=http://127.0.0.1:$PORT
mkdir -p "$W/cfg"
LIBREECHO_NTP_DEFAULTS=./config/ntp.conf ./build/libreecho-web --backend mock --config "$W/cfg/config.json" --mock-config ./config/mock-state.json --web-root ./web --listen "127.0.0.1:$PORT" --seed 42 >"$W/server.log" 2>&1 &
pid=$!
i=0
while ! curl -fsS "$URL/api/v1/status" >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 50 ] || { cat "$W/server.log"; exit 1; }; sleep 0.1; done
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
NTP=$URL/api/v1/system/ntp
put(){ curl -sS -o "$W/out" -w '%{http_code}' -X PUT "$NTP" -H "$CSRF" -H 'Content-Type: application/json' --data "$1"; }
DEFAULTS=time.cloudflare.com,time.nist.gov,ntp1.npl.co.uk,ntp2.npl.co.uk

# Image defaults, no override.
curl -fsS "$NTP" | jq -e --arg d "$DEFAULTS" '.ok and .data.custom == false and .data.servers == "" and .data.defaults == $d' >/dev/null
[ ! -e "$W/cfg/ntp.conf" ]

# A LAN server and an IPv6 address, commas and spaces mixed.
[ "$(put '{"servers":" 192.168.1.1, fe80::1  ntp.lan "}')" = 200 ]
jq -e '.ok and .data.custom and .data.servers == "192.168.1.1,fe80::1,ntp.lan" and .data.applies == "next-start"' "$W/out" >/dev/null
printf 'server 192.168.1.1\nserver fe80::1\nserver ntp.lan\n' | cmp - "$W/cfg/ntp.conf"
[ "$(stat -c %a "$W/cfg/ntp.conf")" = 600 ]
curl -fsS "$NTP" | jq -e '.data.custom and .data.servers == "192.168.1.1,fe80::1,ntp.lan"' >/dev/null

# Rejected without touching the saved file: a fifth peer, shell or URL
# characters, an over-long name, a non-string, and no CSRF header.
for body in '{"servers":"a b c d e"}' '{"servers":"ntp.lan;reboot"}' '{"servers":"http://ntp.lan"}' '{"servers":"$(id)"}' '{"servers":["ntp.lan"]}' '{}' \
    "{\"servers\":\"$(printf 'a%.0s' $(seq 254))\"}"; do
    [ "$(put "$body")" = 400 ] || { echo "accepted: $body" >&2; exit 1; }
    jq -e '.ok == false and .error.code' "$W/out" >/dev/null
done
code=$(curl -sS -o "$W/out" -w '%{http_code}' -X PUT "$NTP" -H 'Content-Type: application/json' --data '{"servers":"evil.example"}')
[ "$code" = 403 ]
code=$(curl -sS -o "$W/out" -w '%{http_code}' -X POST "$NTP" -H "$CSRF" -H 'Content-Type: application/json' --data '{"servers":""}')
[ "$code" = 405 ]
printf 'server 192.168.1.1\nserver fe80::1\nserver ntp.lan\n' | cmp - "$W/cfg/ntp.conf"

# Four peers is the limit timed accepts.
[ "$(put '{"servers":"a.lan,b.lan,c.lan,d.lan"}')" = 200 ]
jq -e '.data.servers == "a.lan,b.lan,c.lan,d.lan"' "$W/out" >/dev/null

# A file timed would reject is reported as no override.
printf 'server bad;peer\n' >"$W/cfg/ntp.conf"
curl -fsS "$NTP" | jq -e '.data.custom == false and .data.servers == ""' >/dev/null

# Empty restores the image defaults by removing the override.
[ "$(put '{"servers":""}')" = 200 ]
jq -e '.data.custom == false and .data.servers == ""' "$W/out" >/dev/null
[ ! -e "$W/cfg/ntp.conf" ]
[ "$(put '{"servers":""}')" = 200 ]
echo "ntp api: ok"
