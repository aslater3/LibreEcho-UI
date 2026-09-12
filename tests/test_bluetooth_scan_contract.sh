#!/bin/sh
set -eu
# A Bluetooth scan has no completion callback: it starts, runs on the
# controller, and ends by itself. The contract the UI depends on is that
# starting a scan clears the previous results, that "scanning" returns to false
# on its own, and that "discovered" then holds what the scan found -- so a
# client that polls GET /api/v1/bluetooth needs nothing else to show the
# result.
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
bt(){ curl -fsS "$URL/api/v1/bluetooth"; }
scan(){ curl -sS -o /tmp/le-bt-scan.out -w '%{http_code}' -X POST "$URL/api/v1/bluetooth/$1" -H "$CSRF" -H 'Content-Type: application/json' --data '{}'; }

# Discovery needs a powered controller; asking for it while off is a conflict,
# not a silent no-op.
curl -fsS -X PUT "$URL/api/v1/bluetooth" -H "$CSRF" -H 'Content-Type: application/json' --data '{"enabled":false}' >/dev/null
[ "$(scan scan)" = 409 ]
jq -e '.error.code=="busy"' /tmp/le-bt-scan.out >/dev/null

curl -fsS -X PUT "$URL/api/v1/bluetooth" -H "$CSRF" -H 'Content-Type: application/json' --data '{"enabled":true}' >/dev/null
bt | jq -e '.data.enabled==true and (.data.discovered|length)==0' >/dev/null

# Starting a scan clears the previous results and reports itself as running.
[ "$(scan scan)" = 200 ]
jq -e '.data.scanning==true and (.data.discovered|length)==0' /tmp/le-bt-scan.out >/dev/null

# The scan ends without anyone stopping it, and the results are there when it
# does. Bounded wait: the mock scan window is a few seconds.
i=0
while bt | jq -e '.data.scanning==true' >/dev/null; do
  i=$((i+1)); [ "$i" -lt 30 ] || { echo 'scan never finished' >&2; bt >&2; exit 1; }
  sleep 0.5
done
bt | jq -e '(.data.discovered|length)>0' >/dev/null
bt | jq -e '.data.discovered[0]|has("address") and has("name") and has("rssi") and has("rssi_valid")' >/dev/null

# Stopping a running scan ends it immediately.
[ "$(scan scan)" = 200 ]
bt | jq -e '.data.scanning==true and (.data.discovered|length)==0' >/dev/null
[ "$(scan scan/stop)" = 200 ]
bt | jq -e '.data.scanning==false' >/dev/null

# The page must announce the end of a scan and must not let a second click on
# the still-labelled "Scanning…" button wipe the list being waited for.
grep -q 'function bluetoothScanFinished' web/js/bluetooth.js
grep -q 'bluetoothScanFinished(b)' web/js/bluetooth.js
grep -q "\$('#bt-scan').disabled=!!b.scanning" web/js/bluetooth.js

echo 'bluetooth scan contract: ok'
