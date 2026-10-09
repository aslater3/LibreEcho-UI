#!/bin/sh
set -eu

# Exercise the real daemon through loopback with the mock backend. The init
# default-copy is represented by copying the shipped defaults into the
# persistent config path; no device paths or services are touched.
PORT=${LIBREECHO_SETUP_TEST_PORT:-18120}
DAEMON=${LIBREECHO_SETUP_TEST_DAEMON:-./build/libreecho-web}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/libreecho-216.XXXXXX")
pid=0

cleanup() {
    if [ "$pid" -gt 1 ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

wait_ready() {
    url=$1
    i=0
    while ! curl -fsS "$url/api/v1/config" >"$TMP/config.json" 2>/dev/null; do
        i=$((i + 1))
        [ "$i" -lt 100 ] || { cat "$TMP/server.log" >&2; exit 1; }
        sleep 0.05
    done
}

start_server() {
    cfg=$1
    users=$2
    port=$3
    "$DAEMON" --backend mock --config "$cfg" \
        --mock-config ./config/mock-state.json --web-root ./web \
        --listen "127.0.0.1:$port" --seed 42 --dev-controls \
        --users-file "$users" >"$TMP/server.log" 2>&1 &
    pid=$!
    wait_ready "http://127.0.0.1:$port"
}

stop_server() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    pid=0
}

assert_incomplete() {
    label=$1
    port=$2
    url="http://127.0.0.1:$port"
    curl -fsS "$url/api/v1/config" >"$TMP/config-$label.json"
    jq -e '.data.setup_completed == false' "$TMP/config-$label.json" >/dev/null
    if [ "${4:-absent}" = absent ]; then
        [ ! -e "$3.setup-complete" ]
    fi
    curl -fsS "$url/" >"$TMP/root-$label.html"
    grep -q 'setup.js' "$TMP/root-$label.html"
    grep -q 'Create your local account.' "$TMP/root-$label.html"
    printf '%s: incomplete setup remains active\n' "$label"
}

# Case 1: this is the production first-start copy from config/defaults.json.
defaults_cfg=$TMP/defaults-copy.json
cp config/defaults.json "$defaults_cfg"
start_server "$defaults_cfg" "$TMP/absent-users-defaults" "$PORT"
assert_incomplete defaults "$PORT" "$defaults_cfg"
stop_server

# Case 2: a partially persisted config without the transaction marker is still
# incomplete, even when the first account already exists.
incomplete_cfg=$TMP/incomplete.json
printf '%s\n' '{"hostname":"partial-echo","volume":41}' >"$incomplete_cfg"
users=$TMP/users
./tools/create-user.sh setup-user setup-password-216 >"$users"
chmod 600 "$users"
start_server "$incomplete_cfg" "$users" "$((PORT + 1))"
assert_incomplete incomplete "$((PORT + 1))" "$incomplete_cfg"
stop_server

# Case 3: a valid marker is the only completion authority and remains so after
# a normal daemon restart. Authentication must still remain enabled.
completed_cfg=$TMP/completed.json
cp config/defaults.json "$completed_cfg"
printf '%s\n' 'schema=1' >"$completed_cfg.setup-complete"
chmod 600 "$completed_cfg.setup-complete"
start_server "$completed_cfg" "$users" "$((PORT + 2))"
url="http://127.0.0.1:$((PORT + 2))"
curl -fsS "$url/api/v1/config" | jq -e \
    '.data.setup_completed == true and
     .data.bootstrap_required == false and
     .data.authentication == "users"' >/dev/null
curl -fsS "$url/" >"$TMP/root-completed-first.html"
grep -q 'LibreEcho Control Centre' "$TMP/root-completed-first.html"
stop_server
start_server "$completed_cfg" "$users" "$((PORT + 2))"
curl -fsS "$url/api/v1/config" | jq -e \
    '.data.setup_completed == true and
     .data.bootstrap_required == false and
     .data.authentication == "users"' >/dev/null
curl -fsS "$url/" >"$TMP/root-completed-restart.html"
grep -q 'LibreEcho Control Centre' "$TMP/root-completed-restart.html"
printf '%s\n' 'completed setup survives restart with authentication enabled'
stop_server

# Case 4: an incomplete setup stays incomplete across a restart. The persisted
# config alone is not completion, so a reboot must not lose the wizard.
restart_cfg=$TMP/restart-incomplete.json
cp config/defaults.json "$restart_cfg"
start_server "$restart_cfg" "$TMP/absent-users-restart" "$((PORT + 3))"
assert_incomplete restart-first "$((PORT + 3))" "$restart_cfg"
stop_server
start_server "$restart_cfg" "$TMP/absent-users-restart" "$((PORT + 3))"
assert_incomplete restart-second "$((PORT + 3))" "$restart_cfg"
printf '%s\n' 'incomplete setup survives restart without a completion marker'
stop_server

# Case 5: a setup transaction that persisted its configuration and then failed
# before writing the marker -- the shape the daemon itself leaves behind on a
# feature-activation failure -- is still incomplete after a restart.
failed_cfg=$TMP/failed-setup.json
printf '%s\n' '{"hostname":"echo","privacy_local_only":false,"privacy_telemetry":false,"wake_word":"LibreEcho","wake_sensitivity":68,"integrations":16}' >"$failed_cfg"
start_server "$failed_cfg" "$TMP/users" "$((PORT + 4))"
assert_incomplete failed-first "$((PORT + 4))" "$failed_cfg"
stop_server
start_server "$failed_cfg" "$TMP/users" "$((PORT + 4))"
assert_incomplete failed-restart "$((PORT + 4))" "$failed_cfg"
printf '%s\n' 'persisted configuration without a marker does not complete setup'
stop_server

# Case 6: only a marker carrying the schema this build understands proves
# completion. A leftover file with an empty or unknown schema must not skip the
# wizard, and must keep the daemon's startup view and the configuration
# worker's view of completion in agreement.
unknown_cfg=$TMP/unknown-schema.json
cp config/defaults.json "$unknown_cfg"
printf '%s\n' 'schema=0' >"$unknown_cfg.setup-complete"
chmod 600 "$unknown_cfg.setup-complete"
start_server "$unknown_cfg" "$TMP/users" "$((PORT + 5))"
assert_incomplete unknown-schema "$((PORT + 5))" "$unknown_cfg" present
stop_server
empty_cfg=$TMP/empty-marker.json
cp config/defaults.json "$empty_cfg"
: >"$empty_cfg.setup-complete"
chmod 600 "$empty_cfg.setup-complete"
start_server "$empty_cfg" "$TMP/users" "$((PORT + 6))"
assert_incomplete empty-marker "$((PORT + 6))" "$empty_cfg" present
printf '%s\n' 'an unrecognised or empty completion marker does not skip setup'
stop_server

printf '%s\n' 'setup first-run defaults/marker regression: ok'
