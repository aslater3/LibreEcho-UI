#!/bin/sh
#
# Factory-reset API guards.
#
# The endpoint is destructive, so its guards are part of the behaviour under
# review rather than incidental plumbing. This drives the real HTTP dispatch --
# not the handler function -- against the mock backend with a real local account
# bootstrapped first, and asserts each refusal on its own: no session, missing
# CSRF, missing confirmation and a wrong confirmation must each refuse the
# request without touching persistent state; the accepted request must actually
# run the reset; and the device-action rate limit must answer 429 as its own
# contract, including for a fully valid request that arrives inside the window.
#
# Every destructive path (reboot, shutdown, factory-reset) arms one shared
# 3-second window in the HTTP layer, before authentication and CSRF are checked,
# so a refused attempt arms it too. The guard checks below are therefore spaced
# past the window, and the two deliberately back-to-back requests are the ones
# that assert the window itself.
set -eu
PORT=${LIBREECHO_RESET_TEST_PORT:-18095}
URL="http://127.0.0.1:$PORT"
CFG=./build/test-reset-api-config.json
USERS=./build/test-reset-api-users
LOG=./build/test-reset-api.log
# The window is 3 seconds measured in whole seconds, so wait past it.
GAP=3.2
# The mock backend's factory default, which the reset restores.
DEFAULT_HOSTNAME=libreecho-dev
# A value written through the API, watched across every attempt below.
FIXTURE_HOSTNAME=reset-fixture-host

make build/libreecho-web >/dev/null
rm -f "$CFG" "$CFG.bak" "$CFG.tmp" "$CFG.setup-complete" "$USERS"

./build/libreecho-web --backend mock --config "$CFG" \
    --mock-config ./config/mock-state.json --web-root ./web \
    --listen "127.0.0.1:$PORT" --seed 42 --dev-controls \
    --users-file "$USERS" >"$LOG" 2>&1 &
pid=$!
cleanup() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$CFG" "$CFG.bak" "$CFG.tmp" "$CFG.setup-complete" "$USERS"
}
trap cleanup EXIT INT TERM

# /api/v1/config is served before an account exists, so it is both the
# readiness probe and the CSRF source.
i=0
while ! curl -fsS "$URL/api/v1/config" >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -lt 30 ] || { cat "$LOG"; exit 1; }
    sleep 0.1
done

CSRFTOKEN=$(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')
[ "${#CSRFTOKEN}" = 64 ]
CSRF="X-LibreEcho-CSRF: $CSRFTOKEN"
CONFIRM='X-LibreEcho-Confirm: confirm-device-action'

expect_code() { # expect_code ACTUAL EXPECTED BODY_FILE
    if [ "$1" != "$2" ]; then
        echo "expected HTTP $2, got $1: $3" >&2
        cat "$3" >&2
        exit 1
    fi
}

assert_error() { # assert_error BODY_FILE EXPECTED_MESSAGE
    jq -e --arg m "$2" \
        '.ok == false and .error.code != null and (.error.message | contains($m))' \
        "$1" >/dev/null
}

# One destructive request. $1 is the output file, the rest are headers.
post_reset() {
    out=$1
    shift
    curl -sS -o "$out" -w '%{http_code}' -X POST \
        "$URL/api/v1/system/factory-reset" -H 'Content-Type: application/json' \
        "$@" --data '{}'
}

# The live hostname, authenticated once an account exists.
hostname_now() {
    if [ -n "${AUTH:-}" ]; then
        curl -fsS "$URL/api/v1/device" -H "$AUTH"
    else
        curl -fsS "$URL/api/v1/device"
    fi | jq -r '.data.hostname'
}

# A refused request must leave the persistent value alone.
assert_fixture_intact() {
    [ "$(hostname_now)" = "$FIXTURE_HOSTNAME" ]
}

# 0. A first-run device requires an account, and the reset is not reachable
#    without one. The reset is a normal authenticated operation after that.
curl -fsS "$URL/api/v1/config" |
    jq -e '.data.authentication == "bootstrap-required" and .data.bootstrap_required == true' >/dev/null
code=$(curl -sS -o /tmp/le-reset-bootstrap.out -w '%{http_code}' \
    -X POST "$URL/api/v1/system/factory-reset" -H "$CSRF" -H "$CONFIRM" \
    -H 'Content-Type: application/json' --data '{}')
expect_code "$code" 401 /tmp/le-reset-bootstrap.out
assert_error /tmp/le-reset-bootstrap.out 'Initial account setup is required'

AUTH="Authorization: Bearer $(curl -fsS -X POST "$URL/api/v1/auth/bootstrap" \
    -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"username":"admin","password":"test-password-123","password_confirm":"test-password-123"}' |
    jq -r '.data.token')"
[ "${#AUTH}" -gt 7 ]
curl -fsS "$URL/api/v1/config" | jq -e '.data.authentication == "users"' >/dev/null

# A live value to watch across every attempt below.
curl -fsS -X PUT "$URL/api/v1/network" -H "$AUTH" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data "{\"hostname\":\"$FIXTURE_HOSTNAME\"}" >/dev/null
[ "$(hostname_now)" = "$FIXTURE_HOSTNAME" ]

# The bootstrap probe above was a destructive path, so it armed the same window
# the checks below assert; wait past it before the first guard check.
sleep "$GAP"
# 1. Missing CSRF token, every other guard satisfied.
code=$(post_reset /tmp/le-reset-nocsrf.out -H "$AUTH" -H "$CONFIRM")
expect_code "$code" 403 /tmp/le-reset-nocsrf.out
assert_error /tmp/le-reset-nocsrf.out 'Missing or invalid CSRF token'
assert_fixture_intact

# 2. The refused request armed the device-action window, so a request that
#    satisfies every guard is answered with the rate limit instead of running.
code=$(post_reset /tmp/le-reset-window.out -H "$AUTH" -H "$CSRF" -H "$CONFIRM")
expect_code "$code" 429 /tmp/le-reset-window.out
assert_error /tmp/le-reset-window.out 'Wait before another device action'
assert_fixture_intact
# Only destructive paths share the window: an ordinary request still works.
curl -fsS -o /dev/null "$URL/api/v1/status" -H "$AUTH"

sleep "$GAP"
# 3. Missing destructive-action confirmation.
code=$(post_reset /tmp/le-reset-noconfirm.out -H "$AUTH" -H "$CSRF")
expect_code "$code" 403 /tmp/le-reset-noconfirm.out
assert_error /tmp/le-reset-noconfirm.out 'confirmation token is required'
assert_fixture_intact

sleep "$GAP"
# 4. A confirmation token that is not the expected one.
code=$(post_reset /tmp/le-reset-badconfirm.out -H "$AUTH" -H "$CSRF" \
    -H 'X-LibreEcho-Confirm: yes')
expect_code "$code" 403 /tmp/le-reset-badconfirm.out
assert_error /tmp/le-reset-badconfirm.out 'confirmation token is required'
assert_fixture_intact

sleep "$GAP"
# 5. No session, confirmation and CSRF satisfied.
code=$(post_reset /tmp/le-reset-noauth.out -H "$CSRF" -H "$CONFIRM")
expect_code "$code" 401 /tmp/le-reset-noauth.out
assert_error /tmp/le-reset-noauth.out 'Authentication is required'
assert_fixture_intact

sleep "$GAP"
# 6. Every guard satisfied: the reset runs and its effect is visible.
code=$(post_reset /tmp/le-reset-accepted.out -H "$AUTH" -H "$CSRF" -H "$CONFIRM")
expect_code "$code" 200 /tmp/le-reset-accepted.out
jq -e '.ok == true and .data.accepted == true' /tmp/le-reset-accepted.out >/dev/null
[ "$(hostname_now)" = "$DEFAULT_HOSTNAME" ]

# 7. A repeat straight after an accepted reset is rate limited, so the reset
#    cannot be run twice inside one window.
code=$(post_reset /tmp/le-reset-double.out -H "$AUTH" -H "$CSRF" -H "$CONFIRM")
expect_code "$code" 429 /tmp/le-reset-double.out
assert_error /tmp/le-reset-double.out 'Wait before another device action'
[ "$(hostname_now)" = "$DEFAULT_HOSTNAME" ]

echo 'factory reset API auth, CSRF, confirmation, rate limit and effect: ok'
