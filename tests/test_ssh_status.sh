#!/bin/sh
# SSH runtime status (#24): GET /network and the diagnostics export report what
# the Platform supervisor is doing, from its status file, without any secret;
# and an existing account with a now-reserved name still loads and signs in, so
# an owner can remove it. Starts its own mock-backend daemon on an isolated port.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${LIBREECHO_SSH_TEST_PORT:-18097}
URL="http://127.0.0.1:$PORT"
BIN="$ROOT/build/libreecho-web"
TMP=$(mktemp -d)
pid=0
cleanup() { [ "$pid" -gt 0 ] && kill "$pid" 2>/dev/null; wait 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM
[ -x "$BIN" ] || { echo "missing $BIN" >&2; exit 1; }

# A users file holding "root", written in the format the daemon itself writes.
salt=0123456789abcdef
digest=$(printf '%s:%s' "$salt" 'root-password-123' | sha256sum | cut -d' ' -f1)
printf 'root:sha256:%s:%s\n' "$salt" "$digest" >"$TMP/users"
chmod 600 "$TMP/users"
printf '{"setup_completed":true}\n' >"$TMP/config.json"

start() {
    [ "$pid" -gt 0 ] && { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null || true; }
    LIBREECHO_SSH_STATUS="$1" LIBREECHO_SSH_INIT="$TMP/no-such-init" \
        "$BIN" --backend mock --config "$TMP/config.json" --web-root "$ROOT/web" \
        --listen "127.0.0.1:$PORT" --seed 42 --users-file "$TMP/users" >"$TMP/server.log" 2>&1 &
    pid=$!
    i=0; while ! curl -fsS "$URL/api/v1/config" >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 50 ] || { cat "$TMP/server.log"; exit 1; }; sleep 0.1; done
    CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
    token=$(curl -fsS -X POST "$URL/api/v1/auth/login" -H "$CSRF" -H 'Content-Type: application/json' \
        --data '{"username":"root","password":"root-password-123"}' | jq -r '.data.token')
    AUTH="Authorization: Bearer $token"
}

cat >"$TMP/ssh.status" <<'STATUS'
schema=1
enabled=1
authentication=webui-users-sha256
account_source=/data/libreecho/config/users
privilege_policy=non-root-ephemeral-users
account_state=ready
host_keys=ephemeral:/tmp/dropbear
STATUS
start "$TMP/ssh.status"
# An existing "root" account still loads and signs in (start() logged in as it).
[ "$token" != null ] && [ -n "$token" ]
curl -fsS "$URL/api/v1/network" -H "$AUTH" | jq -e '.data.ssh_available == true and .data.ssh_state == "ready"' >/dev/null
export_body=$(curl -fsS -X POST "$URL/api/v1/diagnostics/export" -H "$AUTH" -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
printf '%s' "$export_body" | jq -e '.data.ssh == {"available":true,"enabled":true,"account_state":"ready","authentication":"webui-users-sha256","privilege_policy":"non-root-ephemeral-users"}' >/dev/null
printf '%s' "$export_body" | jq -e '.data.manifest.sections | index("ssh") != null' >/dev/null
! printf '%s' "$export_body" | grep -q "$digest"
! printf '%s' "$export_body" | grep -q "$salt"
! printf '%s' "$export_body" | grep -q '/data/libreecho/config/users'

sed -i 's/^enabled=1$/enabled=0/; s/^account_state=ready$/account_state=disabled-by-config/' "$TMP/ssh.status"
curl -fsS "$URL/api/v1/network" -H "$AUTH" | jq -e '.data.ssh_state == "disabled-by-config"' >/dev/null
curl -fsS -X POST "$URL/api/v1/diagnostics/export" -H "$AUTH" -H "$CSRF" -H 'Content-Type: application/json' --data '{}' | jq -e '.data.ssh.enabled == false and .data.ssh.account_state == "disabled-by-config"' >/dev/null

# Anything but a short lowercase token is not reported.
sed -i 's/^account_state=.*$/account_state=ready"><script>/' "$TMP/ssh.status"
curl -fsS "$URL/api/v1/network" -H "$AUTH" | jq -e '.data.ssh_available == true and .data.ssh_state == "stopped"' >/dev/null

# No bundle and no status: SSH is reported as not in this build.
start "$TMP/missing.status"
curl -fsS "$URL/api/v1/network" -H "$AUTH" | jq -e '.data.ssh_available == false and .data.ssh_state == "unavailable"' >/dev/null
curl -fsS -X POST "$URL/api/v1/diagnostics/export" -H "$AUTH" -H "$CSRF" -H 'Content-Type: application/json' --data '{}' | jq -e '.data.ssh == {"available":false}' >/dev/null
echo 'ssh status: ok'
