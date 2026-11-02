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

# DELETE is for pending timers; a ringing timer must be handled by dismiss.
ring_id=$(curl -fsS -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data '{"seconds":1,"label":"ring"}' \
    | jq -r '.data.id')
sleep 2
curl -fsS "$URL/api/v1/timers" | jq -e --argjson id "$ring_id" \
    '.data.timers[] | select(.id == $id) | .state == "ringing"' >/dev/null
code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$URL/api/v1/timers/$ring_id" -H "$CSRF")
[ "$code" = 404 ] || { echo "FAIL: deleting ringing timer returned $code, expected 404"; exit 1; }
curl -fsS -X POST "$URL/api/v1/timers/dismiss" -H "$CSRF" -H "$JSON" --data '{}' >/dev/null
echo "  ringing timers require dismiss: ok"

# Labels are bounded API fields and must be rejected rather than truncated.
long_label=$(python3 -c 'print("x" * 48)')
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data "{\"seconds\":600,\"label\":\"$long_label\"}")
[ "$code" = 400 ] || { echo "FAIL: oversized label returned $code, expected 400"; exit 1; }
echo "  oversized labels refused: ok"

# Decoded JSON control characters are rejected rather than changed in transit.
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data '{"seconds":60,"label":"a\nb"}')
[ "$code" = 400 ] || { echo "FAIL: control-character label returned $code, expected 400"; exit 1; }
echo "  control-character labels refused: ok"

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

# Leading whitespace is part of the label, not formatting to trim. The timerd
# persistence test covers the restart boundary; this checks the API preserves it
# on the immediate round trip too.
curl -fsS -X POST "$URL/api/v1/timers" \
    -H "$CSRF" -H "$JSON" --data '{"seconds":60,"label":"  tea"}' \
    -o /tmp/le-timer-whitespace.out
whitespace_id=$(jq -r '.data.id' < /tmp/le-timer-whitespace.out)
curl -fsS "$URL/api/v1/timers" | jq -e --argjson id "$whitespace_id" \
    '.data.timers[] | select(.id == $id) | .label == "  tea"' >/dev/null
curl -fsS -X DELETE "$URL/api/v1/timers/$whitespace_id" -H "$CSRF" >/dev/null
echo "  leading whitespace labels preserved: ok"
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

# --- weekly alarms and snooze -----------------------------------------------
# A repeating alarm takes a day list and reports the days it repeats on.
code=$(curl -sS -o /tmp/le-alarm-repeat.out -w '%{http_code}' -X POST "$URL/api/v1/alarms" \
    -H "$CSRF" -H "$JSON" --data '{"time":"06:45","label":"school","days":"weekdays"}')
[ "$code" = 201 ] || { echo "FAIL: repeating alarm create returned $code"; cat /tmp/le-alarm-repeat.out; exit 1; }
repeat_id=$(jq -r '.data.id' < /tmp/le-alarm-repeat.out)
jq -e '.data.days == "mon,tue,wed,thu,fri"' < /tmp/le-alarm-repeat.out >/dev/null \
    || { echo "FAIL: repeating alarm did not report its days"; cat /tmp/le-alarm-repeat.out; exit 1; }
curl -fsS "$URL/api/v1/alarms" | jq -e --argjson id "$repeat_id" \
    '.data.alarms[] | select(.id == $id) | .time == "06:45" and .days == "mon,tue,wed,thu,fri"' >/dev/null \
    || { echo "FAIL: alarm list did not show the repeat days"; exit 1; }
echo "  repeating alarm created with its days: ok"

# Days must be a known list; a typo is refused rather than becoming a one-off.
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/alarms" -H "$CSRF" -H "$JSON" \
    --data '{"time":"06:45","days":"funday"}')
[ "$code" = 400 ] || { echo "FAIL: unknown day list returned $code, expected 400"; exit 1; }
echo "  unknown day list refused: ok"

# Snooze is a POST to its own route. With nothing ringing it snoozes nothing.
code=$(curl -sS -o /tmp/le-alarm-snooze.out -w '%{http_code}' -X POST "$URL/api/v1/alarms/snooze" -H "$CSRF")
[ "$code" = 200 ] || { echo "FAIL: snooze returned $code"; cat /tmp/le-alarm-snooze.out; exit 1; }
jq -e '.ok and .data.snoozed == 0' < /tmp/le-alarm-snooze.out >/dev/null \
    || { echo "FAIL: snooze with nothing ringing did not report zero"; cat /tmp/le-alarm-snooze.out; exit 1; }
code=$(curl -sS -o /dev/null -w '%{http_code}' -X GET "$URL/api/v1/alarms/snooze" -H "$CSRF")
[ "$code" = 405 ] || { echo "FAIL: GET /alarms/snooze returned $code, expected 405"; exit 1; }
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/alarms/snooze")
[ "$code" = 403 ] || { echo "FAIL: snooze without CSRF returned $code, expected 403"; exit 1; }
echo "  snooze route, method and CSRF enforced: ok"

code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$URL/api/v1/alarms/$repeat_id" -H "$CSRF")
[ "$code" = 200 ] || { echo "FAIL: repeating alarm cancel returned $code"; exit 1; }
curl -fsS "$URL/api/v1/alarms" | jq -e '(.data.alarms | length) == 0' >/dev/null
echo "  repeating alarm cancelled: ok"

# Alarms run before the capacity test fills every mock slot.
# --- alarms: a wall-clock time, set and cancelled through the same schedule ---
# An empty alarm list is a list. The time must be HH:MM on a 24-hour clock.
curl -fsS "$URL/api/v1/alarms" | jq -e '.ok and (.data.alarms | length) == 0' >/dev/null
echo "  empty alarm list: ok"

for bad in '{"time":"7am"}' '{"time":"7:00:00"}' '{"time":"25:00"}' '{"time":"07:60"}' '{"label":"no time"}'; do
    code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/alarms" \
        -H "$CSRF" -H "$JSON" --data "$bad")
    [ "$code" = 400 ] || { echo "FAIL: alarm body $bad returned $code, expected 400"; exit 1; }
done
echo "  malformed alarm times refused: ok"

code=$(curl -sS -o /tmp/le-alarm-add.out -w '%{http_code}' -X POST "$URL/api/v1/alarms" \
    -H "$CSRF" -H "$JSON" --data '{"time":"06:45","label":"wake"}')
[ "$code" = 201 ] || { echo "FAIL: alarm create returned $code"; cat /tmp/le-alarm-add.out; exit 1; }
alarm_id=$(jq -r '.data.id' < /tmp/le-alarm-add.out)
jq -e '.data.time == "06:45" and (.data.due_epoch > 0)' < /tmp/le-alarm-add.out >/dev/null \
    || { echo "FAIL: alarm create did not return its time"; exit 1; }
curl -fsS "$URL/api/v1/alarms" | jq -e --argjson id "$alarm_id" \
    '.data.alarms as $a | ($a | length) == 1 and $a[0].id == $id and $a[0].time == "06:45" and $a[0].label == "wake"' >/dev/null \
    || { echo "FAIL: alarm list did not show the new alarm"; exit 1; }
# The shared schedule lists the alarm as an alarm, so one timer list stays truthful.
curl -fsS "$URL/api/v1/timers" | jq -e --argjson id "$alarm_id" \
    '.data.timers[] | select(.id == $id) | .kind == "alarm"' >/dev/null
echo "  alarm created, listed by time and kind: ok"

# Alarms cancel only through the alarm route; a countdown id is not an alarm.
code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$URL/api/v1/alarms/$id" -H "$CSRF")
[ "$code" = 404 ] || { echo "FAIL: deleting a countdown through /alarms returned $code"; exit 1; }
code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$URL/api/v1/alarms/$alarm_id" -H "$CSRF")
[ "$code" = 200 ] || { echo "FAIL: alarm cancel returned $code"; exit 1; }
curl -fsS "$URL/api/v1/alarms" | jq -e '(.data.alarms | length) == 0' >/dev/null
echo "  alarm cancelled and removed: ok"

# Reads are GET-only for the alarm list; writes need the CSRF header.
code=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT "$URL/api/v1/alarms" -H "$CSRF" -H "$JSON" --data '{}')
[ "$code" = 405 ] || { echo "FAIL: PUT /alarms returned $code, expected 405"; exit 1; }
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/alarms" -H "$JSON" --data '{"time":"06:45"}')
[ "$code" = 403 ] || { echo "FAIL: alarm create without CSRF returned $code, expected 403"; exit 1; }
echo "  alarm methods and CSRF enforced: ok"

# The Timers page has an alarm form wired to the route.
curl -fsS "$URL/js/app.js" -o /tmp/le-app-alarms.js
grep -q "api/v1/alarms\|'/alarms'\|/alarms/" /tmp/le-app-alarms.js \
    || { echo "FAIL: the timers page does not use the alarm route"; exit 1; }
echo "  alarm form wired into the UI: ok"

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

# Linux timer writes must retain the backend result so operational failures are
# reported as `io` rather than being mislabeled as an absent daemon.
python3 - <<'PY'
from pathlib import Path
source = Path("src/api.c").read_text()
start = source.index('if(!strcmp(p,"/api/v1/timers")&&!strcmp(q->method,"POST"))')
end = source.index('if(!strcmp(p,"/api/v1/device")', start)
block = source[start:end]
# Four: timer create, dismiss, cancel, and the alarm snooze route, which sits in
# this span and uses the same 503 for an unavailable daemon.
assert block.count('err(r,503,rc,"Timer service is unavailable")') == 4
assert 'err(r,503,LE_NOT_SUPPORTED,"Timer service is unavailable")' not in block
PY
echo "  timer write error codes preserved: ok"

echo "timers api: ok"
