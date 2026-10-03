#!/bin/sh
# Focused recovery-backend integration runner (API -> backend_linux -> networkd).
#
# Standalone on purpose: it builds its own private binaries into a unique temp
# directory and never calls `make`, touches `build/`, or runs the aggregate
# suite, so it can run alongside an active aggregate worker without racing it.
# The Makefile wiring (a `test-recovery-backend` target + runner entry) is a
# follow-up the parent owns; see evidence/recovery-backend-integration.md.
#
# Builds three binaries from source, mirroring the Makefile's own compile line
# (`$(CC) $(CPPFLAGS) $(CSTD) $(WARN) $(CFLAGS) -MMD -MP -Isrc -c`):
#   * test-networkd-recovery        real networkd, LE_NETWORKD_TESTING fixture
#   * libreecho-networkd-prod       real networkd, production build (root gate)
#   * libreecho-web-f3              full web daemon with the network socket
#                                   compiled to the private fixture path
#
# Then runs:
#   tests/test_recovery_backend_api.py   HTTP end-to-end mapping over a socket
#   tests/test_recovery_peer_gate.py     production root-peer gate
#
# Usage: sh tests/run_recovery_backend_integration.sh
#   LE_F3_KEEP=1  keep the temp workspace for inspection
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
CC=${CC:-cc}
CSTD=${CSTD:--std=c99}
CFLAGS=${CFLAGS:--O2}
OS_VERSION=$(tr -d '\r\n' < "$ROOT/VERSION" 2>/dev/null || printf unknown)

RUN_DIR=$(mktemp -d "${TMPDIR:-/tmp}/le-recovery-f3.XXXXXX") || exit 1
BIN="$RUN_DIR/bin"
WS="$RUN_DIR/ws"
mkdir -p "$BIN" "$WS"
# The recovery config/secret persist under WS and require a non-group/world
# accessible parent; the fixture workspace must be private.
chmod 700 "$RUN_DIR" "$WS"
SOCK="$WS/network.sock"

cleanup() {
    [ "${LE_F3_KEEP:-0}" = "1" ] || rm -rf "$RUN_DIR"
}
trap cleanup EXIT INT TERM

# Mirror the Makefile's CPPFLAGS (web build). -DLE_DEV_CONTROLS=1 matches the
# dev image the aggregate harness builds; it does not affect recovery routes.
CPP="-D_POSIX_C_SOURCE=200809L -Isrc/adapter -Isrc -DLE_TLS_AVAILABLE=0 -DLE_DEV_CONTROLS=1"
CPP="$CPP -DLE_OS_VERSION=\"$OS_VERSION\" -DLE_SOURCE_COMMIT=\"test\" -DLE_SOURCE_DIRTY=\"test\" -DLE_SOURCE_DIGEST=\"test\""

ND_SOURCES="src/adapter/networkd.c src/adapter/network_health.c src/adapter/gateway_probe.c src/adapter/adapter_server.c src/log.c src/adapter/network_recovery.c"
WEB_SOURCES="src/main.c src/http_server.c src/inherited_fds.c src/tls_stub.c src/api.c src/update_identity.c src/diagnostic_export.c src/feature_provenance.c src/authority_provenance.c src/factory_reset.c src/auth.c src/backend.c src/backend_mock.c src/backend_linux.c src/config_store.c src/event_bus.c src/json.c src/log.c src/service_env.c src/adapter/adapter_client.c src/adapter/adapter_server.c src/adapter/wyoming_client.c src/adapter/voice_stream.c"

echo "== building private recovery-backend binaries =="
cd "$ROOT" || exit 1

# networkd fixture (LE_NETWORKD_TESTING) and production build (root gate active)
"$CC" -D_POSIX_C_SOURCE=200809L -DLE_NETWORKD_TESTING $CSTD -Wall -Wextra -Wpedantic -Werror \
    -Isrc -Isrc/adapter $ND_SOURCES -o "$BIN/test-networkd-recovery" || exit 1
"$CC" -D_POSIX_C_SOURCE=200809L $CSTD -Wall -Wextra -Wpedantic -Werror \
    -Isrc -Isrc/adapter $ND_SOURCES -o "$BIN/libreecho-networkd-prod" || exit 1

# Full web daemon: only backend_linux.c needs the socket override.
web_status=0
WEB_OBJS=""
for source in $WEB_SOURCES; do
    object="$BIN/$(basename "$source" .c).o"
    extra=""
    [ "$source" = "src/backend_linux.c" ] && extra="-DLE_ADAPTER_NETWORK_SOCK=\"$SOCK\""
    if ! "$CC" $CPP $extra $CSTD $CFLAGS -MMD -MP -c "$source" -o "$object" 2>"$BIN/$(basename "$source").log"; then
        echo "compile failed: $source" >&2
        cat "$BIN/$(basename "$source").log" >&2
        web_status=1
        break
    fi
    WEB_OBJS="$WEB_OBJS $object"
done
if [ "$web_status" != 0 ]; then
    echo "recovery-backend-integration: build failed" >&2
    exit 1
fi
"$CC" $CFLAGS $WEB_OBJS -lm -lpthread -o "$BIN/libreecho-web-f3" || exit 1

echo "== running API -> backend_linux -> networkd mapping =="
LE_F3_WEB="$BIN/libreecho-web-f3" \
LE_F3_NETWORKD="$BIN/test-networkd-recovery" \
LE_F3_WORKSPACE="$WS" \
python3 "$ROOT/tests/test_recovery_backend_api.py"
api_status=$?

echo "== running production root-peer gate =="
LE_F3_NETWORKD_PROD="$BIN/libreecho-networkd-prod" \
python3 "$ROOT/tests/test_recovery_peer_gate.py"
gate_status=$?

if [ "$api_status" = 0 ] && [ "$gate_status" = 0 ]; then
    echo "recovery-backend-integration-ok"
    exit 0
fi
echo "recovery-backend-integration-failures: api=$api_status gate=$gate_status"
exit 1
