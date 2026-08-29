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

# The path component must be an entire nonzero uint; a numeric prefix must not
# alias the live timer and cancel it.
for malformed in "${id}junk" "18446744073709551616"; do
    code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$URL/api/v1/timers/$malformed" -H "$CSRF")
    [ "$code" = 404 ] || { echo "FAIL: malformed timer id $malformed returned $code, expected 404"; exit 1; }
done
curl -fsS "$URL/api/v1/timers" | jq -e --argjson id "$id" '.data.timers | length == 1 and .[0].id == $id' >/dev/null
echo "  malformed timer ids refused without cancellation: ok"

# Labels are bounded API fields and must be rejected rather than truncated.
long_label=$(python3 -c 'print("x" * 48)')
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data "{\"seconds\":600,\"label\":\"$long_label\"}")
[ "$code" = 400 ] || { echo "FAIL: oversized label returned $code, expected 400"; exit 1; }
echo "  oversized labels refused: ok"

# Timer fields must come from the request object's top level, not a nested
# metadata object that happens to use the same names.
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" \
    --data '{"meta":{"seconds":1},"seconds":999999}')
[ "$code" = 400 ] || { echo "FAIL: nested seconds returned $code, expected 400"; exit 1; }
# JSON serializers commonly emit non-ASCII labels as Unicode escapes. The
# decoded UTF-8 bytes still count toward the 47-byte server limit.
curl -fsS -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data '{"seconds":60,"label":"caf\u00e9"}' \
    -o /tmp/le-timer-unicode.out
unicode_id=$(jq -r '.data.id' < /tmp/le-timer-unicode.out)
curl -fsS "$URL/api/v1/timers" | jq -e --argjson id "$unicode_id" \
    '.data.timers[] | select(.id == $id) | .label == "café"' >/dev/null
curl -fsS -X DELETE "$URL/api/v1/timers/$unicode_id" -H "$CSRF" >/dev/null
echo "  top-level fields and Unicode labels: ok"

# Raw UTF-8 labels are accepted when their byte sequence is valid.
python3 -c 'from pathlib import Path; q=bytes((34,)); Path("/tmp/le-timer-valid-utf8.json").write_bytes(b"{" + q + b"seconds" + q + b":60," + q + b"label" + q + b":" + q + b"caf" + bytes((0xc3, 0xa9)) + q + b"}")'
curl -fsS -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data-binary @/tmp/le-timer-valid-utf8.json \
    -o /tmp/le-timer-valid-utf8.out
valid_utf8_id=$(jq -r '.data.id' < /tmp/le-timer-valid-utf8.out)
curl -fsS "$URL/api/v1/timers" | jq -e --argjson id "$valid_utf8_id" \
    '.data.timers[] | select(.id == $id) | .label == "café"' >/dev/null
curl -fsS -X DELETE "$URL/api/v1/timers/$valid_utf8_id" -H "$CSRF" >/dev/null

# Exactly 47 UTF-8 bytes remain valid; the limit is bytes, not code points.
python3 -c 'from pathlib import Path; q=bytes((34,)); label=bytes((0xf0,0x9f,0x98,0x80))*11+b"abc"; Path("/tmp/le-timer-47-byte.json").write_bytes(b"{" + q + b"seconds" + q + b":60," + q + b"label" + q + b":" + q + label + q + b"}")'
curl -fsS -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data-binary @/tmp/le-timer-47-byte.json \
    -o /tmp/le-timer-47-byte.out
boundary_id=$(jq -r '.data.id' < /tmp/le-timer-47-byte.out)
[ "$boundary_id" != "null" ] || { echo "FAIL: valid 47-byte label was rejected"; exit 1; }
curl -fsS "$URL/api/v1/timers" | jq -e --argjson id "$boundary_id" \
    '.data.timers[] | select(.id == $id) | (.label | length) == 14' >/dev/null
curl -fsS -X DELETE "$URL/api/v1/timers/$boundary_id" -H "$CSRF" >/dev/null
echo "  valid 47-byte Unicode labels accepted: ok"

after_valid_utf8=$(python3 -c 'from pathlib import Path; q=bytes((34,)); base=b"{" + q + b"seconds" + q + b":60," + q + b"label" + q + b":" + q + b"bad "; cases={"continuation":bytes((0x80,)),"overlong":bytes((0xc0,0xaf)),"truncated":bytes((0xe2,0x82)),"bad-continuation":bytes((0xe2,0x28,0xa1)),"surrogate":bytes((0xed,0xa0,0x80)),"out-of-range":bytes((0xf4,0x90,0x80,0x80))}; [Path("/tmp/le-timer-invalid-"+name+".json").write_bytes(base+value+q+b"}") for name,value in cases.items()]; print(" ".join(cases))')
for malformed in $after_valid_utf8; do
    code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers" \
        -H "$CSRF" -H "$JSON" --data-binary "@/tmp/le-timer-invalid-$malformed.json")
    [ "$code" = 400 ] || { echo "FAIL: malformed UTF-8 label $malformed returned $code, expected 400"; exit 1; }
done
echo "  malformed UTF-8 labels refused: ok"

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

# The fixed mock schedule has sixteen slots. A valid request beyond that hard
# limit is busy, not an invalid duration or an unavailable service.
i=0
while [ "$i" -lt 16 ]; do
    code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers" \
        -H "$CSRF" -H "$JSON" --data '{"seconds":600}')
    [ "$code" = 201 ] || { echo "FAIL: capacity fill returned $code, expected 201"; exit 1; }
    i=$((i + 1))
done
code=$(curl -sS -o /tmp/le-timer-capacity.out -w '%{http_code}' -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data '{"seconds":600}')
[ "$code" = 409 ] || { echo "FAIL: full timer schedule returned $code, expected 409"; exit 1; }
jq -e '.error.code == "busy"' /tmp/le-timer-capacity.out >/dev/null
echo "  timer capacity reports busy: ok"

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
