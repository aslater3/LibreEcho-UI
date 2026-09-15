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
    [ ! -e "$3.setup-complete" ]
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

printf '%s\n' 'setup first-run defaults/marker regression: ok'
