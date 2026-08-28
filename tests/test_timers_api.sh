#!/bin/sh
# The timers HTTP API, against the mock backend.
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
JSON='Content-Type: application/json'

# --- an empty schedule is a list, not an error ----------------------------
curl -fsS "$URL/api/v1/timers" | jq -e '.ok and (.data.timers | length) == 0' >/dev/null
echo "  empty schedule listed: ok"

# --- creating one returns its id and 201 ----------------------------------
code=$(curl -sS -o /tmp/le-timer-add.out -w '%{http_code}' -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data '{"seconds":600,"label":"pasta"}')
[ "$code" = 201 ] || { echo "FAIL: create returned $code"; cat /tmp/le-timer-add.out; exit 1; }
id=$(jq -r '.data.id' < /tmp/le-timer-add.out)
[ -n "$id" ] && [ "$id" != "null" ] || { echo "FAIL: no id returned"; exit 1; }
echo "  timer created: ok"

curl -fsS "$URL/api/v1/timers" \
    | jq -e --argjson id "$id" '.data.timers | length == 1 and .[0].id == $id and .[0].label == "pasta"' >/dev/null
echo "  timer listed with its label: ok"

# Labels are bounded API fields and must be rejected rather than truncated.
long_label=$(python3 -c 'print("x" * 48)')
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data "{\"seconds\":600,\"label\":\"$long_label\"}")
[ "$code" = 400 ] || { echo "FAIL: oversized label returned $code, expected 400"; exit 1; }
echo "  oversized labels refused: ok"

# The API must reject values outside the bounded countdown range before a
# backend-specific timer implementation sees them.
for body in '{"seconds":0}' '{"seconds":-5}' '{"seconds":999999999}' \
            '{"seconds":4294967297}'; do
    code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers" \
        -H "$CSRF" -H "$JSON" --data "$body")
    [ "$code" = 400 ] || { echo "FAIL: $body returned $code, expected 400"; exit 1; }
done
# A missing length is also a 400 rather than a timer of zero.
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data '{"label":"no length"}')
[ "$code" = 400 ] || { echo "FAIL: missing seconds returned $code"; exit 1; }
echo "  out-of-range and missing lengths refused: ok"

# --- cancelling -----------------------------------------------------------
code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$URL/api/v1/timers/$id" -H "$CSRF")
[ "$code" = 200 ] || { echo "FAIL: cancel returned $code"; exit 1; }
curl -fsS "$URL/api/v1/timers" | jq -e '(.data.timers | length) == 0' >/dev/null
echo "  timer cancelled: ok"

# Cancelling it again is a 404: the id is gone, and saying "ok" would let a
# stale page believe it removed something it did not.
code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$URL/api/v1/timers/$id" -H "$CSRF")
[ "$code" = 404 ] || { echo "FAIL: cancelling twice returned $code, expected 404"; exit 1; }
echo "  cancelling a gone timer is 404: ok"

# Each timer route has an explicit method contract rather than falling through
# to a misleading 404.
code=$(curl --no-fail -sS -o /dev/null -w '%{http_code}' -X PUT "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data '{}')
[ "$code" = 405 ] || { echo "FAIL: PUT /timers returned $code, expected 405"; exit 1; }
code=$(curl --no-fail -sS -o /dev/null -w '%{http_code}' -X GET "$URL/api/v1/timers/dismiss")
[ "$code" = 405 ] || { echo "FAIL: GET /timers/dismiss returned $code, expected 405"; exit 1; }
code=$(curl --no-fail -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers/$id" \
    -H "$CSRF" -H "$JSON" --data '{}')
[ "$code" = 405 ] || { echo "FAIL: POST /timers/{id} returned $code, expected 405"; exit 1; }
echo "  unsupported timer methods refused: ok"

# --- dismiss reports how many rings it stopped ----------------------------
curl -fsS -X POST "$URL/api/v1/timers/dismiss" -H "$CSRF" -H "$JSON" --data '{}' \
    | jq -e '.ok and .data.dismissed == 0' >/dev/null
echo "  dismiss with nothing ringing reports zero: ok"

# --- writes require the CSRF header ---------------------------------------
# Timers are a write surface like any other; a page on another origin must not
# be able to set one.
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers" \
    -H "$JSON" --data '{"seconds":60}')
[ "$code" = 403 ] || { echo "FAIL: create without CSRF returned $code, expected 403"; exit 1; }
echo "  writes require CSRF: ok"

# --- the page is reachable and names the feature --------------------------
# A route with no way in from the UI is a feature nobody finds.
# Fetched once to a file: piping a large response into grep -q closes the
# pipe early and makes curl report a write failure that is not one.
curl -fsS "$URL/js/app.js" -o /tmp/le-app.js
grep -q "timersPage" /tmp/le-app.js \
    || { echo "FAIL: the web app has no timers page"; exit 1; }
grep -q "\['Timers'," /tmp/le-app.js \
    || { echo "FAIL: timers are missing from the navigation"; exit 1; }
echo "  timers page is wired into the UI: ok"

echo "timers api: ok"
