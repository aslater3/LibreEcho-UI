#!/bin/sh
# Boot-chain status and opt-in repair API (0.13.21 bridge), exercised against
# a fake libreecho-boot-chain selected through LIBREECHO_BOOT_CHAIN.
set -eu
PORT=${LIBREECHO_BOOT_CHAIN_TEST_PORT:-18089}
URL="http://127.0.0.1:$PORT"
ROOT=./build/test-boot-chain
CFG=$ROOT/config.json
TOOL=$ROOT/libreecho-boot-chain
rm -rf "$ROOT"
mkdir -p "$ROOT"
printf '{}\n' >"$CFG"
printf '%s\n' available >"$ROOT/mode"
cat >"$TOOL" <<'EOF'
#!/bin/sh
dir=$(dirname "$0")
mode=$(cat "$dir/mode")
printf '%s\n' "$*" >"$dir/argv"
emit_status(){
    printf '%s\n' schema=1 layout=legacy-amonet chain=legacy-0.13 board=radar_puffin \
        board_confidence=confirmed board_evidence=idme-prefix:radar,gpt-end:radar \
        expdb=fastboot-marker "repair=$1" "reason=$2" package=amonet-radar-v1.0.0.zip \
        "phase=$3" recovery=other profile=ota emmc_life=0x01 future_key=ignored
}
case "$1:$mode" in
status:available) printf 'note: probing\n'; emit_status available '' '' ;;
status:in-progress) emit_status in-progress '' writing-recovery ;;
status:kaeru) printf '%s\n' schema=1 layout=stock chain=kaeru-radar-v1 board=radar_puffin \
        board_confidence=confirmed expdb=kaeru repair=not-needed recovery=twrp-radar ;;
status:unconfirmed) printf '%s\n' schema=1 chain=legacy-0.13 board=unknown \
        board_confidence=unconfirmed repair=available ;;
status:unknown-enum) printf '%s\n' schema=1 chain=kaeru-future-v9 board=radar_puffin \
        board_confidence=confirmed repair=sideways recovery=twrp-future profile=weird ;;
status:bad-token) printf '%s\n' schema=1 chain=legacy-0.13 repair=available 'reason=has space' ;;
status:bad-quote) printf '%s\n' schema=1 chain=legacy-0.13 'repair=available"' ;;
status:duplicate) printf '%s\n' schema=1 chain=legacy-0.13 repair=available repair=blocked ;;
status:no-schema) printf '%s\n' chain=legacy-0.13 repair=available ;;
status:schema2) printf '%s\n' schema=2 chain=legacy-0.13 repair=available ;;
status:long) printf 'schema=1\nchain=legacy-0.13\nrepair=available\nphase=%s\n' \
        "$(printf '%0100d' 0)" ;;
status:overflow) i=0; while [ "$i" -lt 400 ]; do printf 'noise-line-%040d\n' "$i"; i=$((i + 1)); done ;;
status:fail) printf 'ERROR:emmc_readonly\n' >&2; exit 1 ;;
repair-start:started)
    # Detach like the real tool: the worker keeps stdout open well past exit.
    ( sleep 5 ) &
    printf '%s\n' "$!" >"$dir/detached.pid"
    printf 'started=1\n' ;;
repair-start:refused) printf 'ERROR:board_mismatch\n'; exit 1 ;;
repair-start:silent) exit 0 ;;
repair-start:crash) exit 3 ;;
*) printf 'ERROR:unexpected\n'; exit 2 ;;
esac
EOF
chmod 0755 "$TOOL"
LIBREECHO_BOOT_CHAIN=$TOOL \
./build/libreecho-web --backend linux --config "$CFG" --web-root ./web \
    --listen "127.0.0.1:$PORT" >"$ROOT/server.log" 2>&1 &
pid=$!
cleanup(){
    kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true
    [ -f "$ROOT/detached.pid" ] && kill "$(cat "$ROOT/detached.pid")" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
i=0
while ! csrf=$(curl -fsS "$URL/api/v1/config" 2>/dev/null | jq -er .data.csrf_token); do
    i=$((i + 1)); [ "$i" -lt 100 ] || { echo 'boot-chain test server did not start' >&2; exit 1; }
    sleep 0.1
done
mode(){ printf '%s\n' "$1" >"$ROOT/mode"; }
get(){ curl -sS -m 30 -o "$ROOT/out.json" -w '%{http_code}' "$URL/api/v1/system/boot-chain"; }
repair(){ # $1 csrf, $2 confirm header, $3 body
    curl -sS -m 30 -o "$ROOT/out.json" -w '%{http_code}' -X POST \
        -H "X-LibreEcho-CSRF: $1" -H "X-LibreEcho-Confirm: $2" \
        -H 'Content-Type: application/json' --data "$3" "$URL/api/v1/system/boot-chain/repair"
}
body='{"confirm":"REPAIR-BOOTLOADER","board":"radar_puffin"}'

# Status: documented keys only, unknown keys ignored, stray lines tolerated.
mode available
[ "$(get)" = 200 ]
jq -e '.ok and .error == null and .data.supported and .data.schema == 1 and
    .data.layout == "legacy-amonet" and .data.chain == "legacy-0.13" and
    .data.board == "radar_puffin" and .data.board_confidence == "confirmed" and
    .data.board_evidence == "idme-prefix:radar,gpt-end:radar" and
    .data.expdb == "fastboot-marker" and .data.repair == "available" and
    .data.reason == "" and .data.package == "amonet-radar-v1.0.0.zip" and
    .data.recovery == "other" and .data.profile == "ota" and
    .data.emmc_life == "0x01" and .data.twrp_present == false and
    (.data | has("future_key") | not)' "$ROOT/out.json" >/dev/null
[ "$(cat "$ROOT/argv")" = status ]
mode kaeru
[ "$(get)" = 200 ]
jq -e '.data.chain == "kaeru-radar-v1" and .data.repair == "not-needed" and
    .data.recovery == "twrp-radar" and .data.twrp_present == true' "$ROOT/out.json" >/dev/null
# An unconfirmed board never carries a repair offer, whatever the tool says.
mode unconfirmed
[ "$(get)" = 200 ]
jq -e '.data.repair == "blocked" and .data.reason == "board_unconfirmed" and
    .data.board == "unknown"' "$ROOT/out.json" >/dev/null
# Enumerated values the daemon does not know collapse to safe fallbacks.
mode unknown-enum
[ "$(get)" = 200 ]
jq -e '.data.chain == "unknown" and .data.repair == "unknown" and
    .data.recovery == "unknown" and .data.profile == "unknown" and
    .data.twrp_present == false' "$ROOT/out.json" >/dev/null
# Malformed tool output is a 502, never passed through or truncated.
for m in bad-token bad-quote duplicate no-schema schema2 long overflow; do
    mode "$m"
    code=$(get)
    [ "$code" = 502 ] || { echo "boot-chain $m: expected 502, got $code" >&2; exit 1; }
    jq -e '.ok == false and .data == null and .error.code == "io_error"' "$ROOT/out.json" >/dev/null
done
mode fail
[ "$(get)" = 503 ]
jq -e '.ok == false and .error.reason == "emmc_readonly"' "$ROOT/out.json" >/dev/null
code=$(curl -sS -o "$ROOT/out.json" -w '%{http_code}' -X PUT -H "X-LibreEcho-CSRF: $csrf" \
    -H 'Content-Type: application/json' --data '{}' "$URL/api/v1/system/boot-chain")
[ "$code" = 405 ]

# Repair: CSRF, device-action confirmation and the exact body are all required.
mode started
rm -f "$ROOT/argv"
[ "$(repair wrong confirm-device-action "$body")" = 403 ]
[ "$(repair "$csrf" '' "$body")" = 403 ]
[ "$(repair "$csrf" wrong "$body")" = 403 ]
[ "$(repair "$csrf" confirm-device-action '{"confirm":"repair-bootloader","board":"radar_puffin"}')" = 400 ]
[ "$(repair "$csrf" confirm-device-action '{"board":"radar_puffin"}')" = 400 ]
[ "$(repair "$csrf" confirm-device-action '{"confirm":"REPAIR-BOOTLOADER"}')" = 400 ]
[ "$(repair "$csrf" confirm-device-action '{"confirm":"REPAIR-BOOTLOADER","board":"unknown"}')" = 400 ]
[ "$(repair "$csrf" confirm-device-action '{"confirm":"REPAIR-BOOTLOADER","board":"radar_puffin","board":"biscuit"}')" = 400 ]
[ "$(repair "$csrf" confirm-device-action '{"nested":{"confirm":"REPAIR-BOOTLOADER","board":"radar_puffin"}}')" = 400 ]
[ "$(repair "$csrf" confirm-device-action '')" = 400 ]
[ ! -f "$ROOT/argv" ] || { echo 'repair tool ran for a rejected request' >&2; exit 1; }
code=$(curl -sS -o "$ROOT/out.json" -w '%{http_code}' "$URL/api/v1/system/boot-chain/repair")
[ "$code" = 405 ]
# Accepted: 202 promptly, even though the detached worker holds the pipe.
start=$(date +%s)
[ "$(repair "$csrf" confirm-device-action "$body")" = 202 ]
[ $(( $(date +%s) - start )) -lt 4 ] || { echo 'repair-start waited on the detached worker' >&2; exit 1; }
jq -e '.ok and .data.started == true and .data.repair == "in-progress"' "$ROOT/out.json" >/dev/null
[ "$(cat "$ROOT/argv")" = 'repair-start --board radar_puffin' ]
[ "$(repair "$csrf" confirm-device-action '{"board":"biscuit","confirm":"REPAIR-BOOTLOADER"}')" = 202 ]
[ "$(cat "$ROOT/argv")" = 'repair-start --board biscuit' ]
mode in-progress
[ "$(get)" = 200 ]
jq -e '.data.repair == "in-progress" and .data.phase == "writing-recovery"' "$ROOT/out.json" >/dev/null
# Tool refusal carries its reason token; other failures do not invent one.
mode refused
[ "$(repair "$csrf" confirm-device-action "$body")" = 409 ]
jq -e '.ok == false and .data == null and .error.code == "busy" and
    .error.reason == "board_mismatch"' "$ROOT/out.json" >/dev/null
mode silent
[ "$(repair "$csrf" confirm-device-action "$body")" = 502 ]
mode crash
[ "$(repair "$csrf" confirm-device-action "$body")" = 503 ]
jq -e '.ok == false and (.error | has("reason") | not)' "$ROOT/out.json" >/dev/null

# Absent (non-executable) tool: not supported, like OTA on a development image.
chmod 0644 "$TOOL"
[ "$(get)" = 501 ]
jq -e '.ok == false and .error.code == "not_supported"' "$ROOT/out.json" >/dev/null
[ "$(repair "$csrf" confirm-device-action "$body")" = 501 ]
cleanup
trap - EXIT INT TERM

# The mock backend never offers the repair.
LIBREECHO_BOOT_CHAIN=$TOOL ./build/libreecho-web --backend mock --config "$CFG" --web-root ./web \
    --listen "127.0.0.1:$PORT" --seed 42 >"$ROOT/mock.log" 2>&1 &
pid=$!
trap cleanup EXIT INT TERM
chmod 0755 "$TOOL"
mode available
i=0
while ! curl -fsS "$URL/api/v1/config" >/dev/null 2>&1; do
    i=$((i + 1)); [ "$i" -lt 100 ] || { echo 'mock server did not start' >&2; exit 1; }
    sleep 0.1
done
[ "$(get)" = 501 ]

# OTA v3 (0.14) update status: GET /system/update reads hard-coded device
# paths, so pin the bounded optional-key parsing at source level.
python3 - <<'PY'
from pathlib import Path
api = Path('src/api.c').read_text()
fn = api[api.index('static void update_status_json'):api.index('struct boot_chain_status{')]
for key in ('ota_format', 'target_release'):
    assert f'"/data/libreecho/update/check-status","{key}"' in fn
    assert f'"/data/libreecho/update/pending","{key}"' in fn
assert 'strcmp(latest_format,"v1")&&strcmp(latest_format,"v2")&&strcmp(latest_format,"v3")' in fn
assert 'boot_chain_token(latest_target,strlen(latest_target),65)' in fn
assert 'boot_chain_token(pending_target,strlen(pending_target),65)' in fn
assert 'else if(!strcmp(cap,"immutable-target-v3"))immutable_v3=1;' in fn
for field in ('latest_ota_format', 'latest_target_release', 'pending_ota_format',
              'pending_target_release', 'immutable_target_v3'):
    assert field in fn
# v1/v2 keys and states are untouched.
assert '"/data/libreecho/update/pending","version",version' in fn
assert '"/data/libreecho/update/check-status","latest_version"' in fn
PY
echo 'boot-chain API: ok'
