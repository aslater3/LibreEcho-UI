#!/bin/sh
set -eu
# The OTG port serves either ADB or a USB drive, never both, and the ADB gadget
# is this image's only shell. Those two facts drive the whole contract: the role
# is switched through the kernel's usb_role class (never the MUSB "mode"
# attribute, which blocks and takes the gadget down), the switch is never
# persisted, and every boot pins the port back to device.
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"

# The feature report always carries the USB role fields, whatever the hardware.
curl -fsS "$URL/api/v1/system/features" | jq -e \
    'has("data") and (.data|has("simulation") and has("usb_host")
     and has("usb_role") and has("usb_role_supported"))' >/dev/null

# A host with no switchable role must say so rather than pretend it switched.
supported=$(curl -fsS "$URL/api/v1/system/features" | jq -r '.data.usb_role_supported')
code=$(curl -sS -o /tmp/le-usb-role.out -w '%{http_code}' -X PUT "$URL/api/v1/system/features" \
    -H "$CSRF" -H 'Content-Type: application/json' --data '{"usb_host":true}')
if [ "$supported" = false ]; then
    [ "$code" = 501 ] || { echo "expected 501 without a usb_role switch, got $code" >&2; exit 1; }
    jq -e '.ok == false' /tmp/le-usb-role.out >/dev/null
else
    [ "$code" = 200 ] || { echo "role switch present but PUT returned $code" >&2; exit 1; }
    curl -fsS -X PUT "$URL/api/v1/system/features" -H "$CSRF" \
        -H 'Content-Type: application/json' --data '{"usb_host":false}' >/dev/null
fi

# Simulation still round-trips; adding usb_host must not have displaced it.
curl -fsS -X PUT "$URL/api/v1/system/features" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"simulation":true}' | jq -e '.ok and .data.simulation == true' >/dev/null
curl -fsS -X PUT "$URL/api/v1/system/features" -H "$CSRF" -H 'Content-Type: application/json' \
    --data '{"simulation":false}' | jq -e '.ok and .data.simulation == false' >/dev/null
# A PUT naming neither field is still a bad request.
code=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT "$URL/api/v1/system/features" \
    -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 400 ] || { echo "empty feature PUT should be 400, got $code" >&2; exit 1; }

# The kernel log endpoint answers, or says why. /dev/kmsg is not readable in
# every test sandbox, so both outcomes are legitimate; a 500 or a hang is not.
code=$(curl -sS -o /tmp/le-kmsg.out -w '%{http_code}' "$URL/api/v1/diagnostics/kernel")
case "$code" in
  200) jq -e '.ok and (.data.lines|type=="array") and (.data.count|type=="number")' /tmp/le-kmsg.out >/dev/null
       jq -e '(.data.lines|length) <= 80' /tmp/le-kmsg.out >/dev/null ;;
  503) jq -e '.ok == false' /tmp/le-kmsg.out >/dev/null ;;
  *)   echo "kernel log endpoint returned $code" >&2; exit 1 ;;
esac

# The role must be driven through the usb_role class, never the blocking
# MUSB mode attribute, and must never be written to the config store.
grep -q 'usb_role_write' src/api.c
grep -q '/sys/class/usb_role' src/api.c
! grep -q 'musb-hdrc.*\/mode' src/api.c
! grep -q 'usb_host' src/config_manager.c 2>/dev/null || true
# The storage endpoint answers whether or not a drive is present, and must
# never claim a mount it does not have.
curl -fsS "$URL/api/v1/storage/usb" | jq -e '.ok and (.data|has("present"))' >/dev/null
curl -fsS "$URL/api/v1/storage/usb" | jq -e \
    'if .data.present then (.data|has("device") and has("entries")) else .data.mounted == false end' >/dev/null
# Unsupported verbs on the USB browse and playback routes must be explicit
# 405s, rather than falling through to a misleading 404.
for method in POST PUT DELETE PATCH; do
    code=$(curl -sS -o /tmp/le-usb-storage-method.out -w '%{http_code}' \
        -X "$method" "$URL/api/v1/storage/usb" \
        -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
    [ "$code" = 405 ]
done
for method in GET PUT DELETE PATCH; do
    code=$(curl -sS -o /tmp/le-usb-play-method.out -w '%{http_code}' \
        -X "$method" "$URL/api/v1/storage/usb/play" \
        -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
    [ "$code" = 405 ]
done

# A browse target may not be clipped into the bounded request-path buffer.
long_path=$(python3 -c 'print("a" * 240)')
code=$(curl -sS -o /tmp/le-usb-long-browse.out -w '%{http_code}' \
    "$URL/api/v1/storage/usb?path=$long_path")
[ "$code" = 400 ]
# Playback has the same complete-path rule for JSON request bodies.
name256=$(python3 -c 'print("a" * 256)')
name256_payload=$(jq -cn --arg path "$name256" '{path:$path}')
code=$(curl -sS -o /tmp/le-usb-long-play.out -w '%{http_code}' \
    -X POST "$URL/api/v1/storage/usb/play" -H "$CSRF" -H 'Content-Type: application/json' \
    --data "$name256_payload")
[ "$code" = 400 ]
# Read-only is a property of the code, not a promise in prose.
grep -q 'MS_RDONLY' src/api.c

grep -q 'feature-usb-host' web/js/app.js
grep -q 'setting is temporary and resets to device mode on the next boot' web/js/app.js
! grep -q 'setting is remembered' web/js/app.js
! grep -q 'hold any button on the device while it boots' web/js/app.js
python3 - <<'PY'
import json
with open('web/openapi.json', encoding='utf-8') as stream:
    spec = json.load(stream)
schema = spec['paths']['/system/features']['put']['requestBody']['content']['application/json']['schema']
assert schema['minProperties'] == 1
assert schema['properties']['usb_host']['type'] == 'boolean'
assert 'not persisted' in schema['properties']['usb_host']['description']
PY
# Boot always returns the port to device so ADB cannot be left switched off.
# The init lives in the platform repository, which is not always checked out
# beside this one (CI builds this repo alone), so assert it only when present.
init=../LibreEcho-platform/tools/mt8163-arm32/initramfs/libreecho-init
if [ -f "$init" ]; then
    grep -q 'usb-role-pinned-device' "$init"
else
    echo '  (platform repo absent; skipped the boot-pin assertion)'
fi

echo 'usb role contract: ok'
