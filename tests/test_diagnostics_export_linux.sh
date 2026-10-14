#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
bundle=$(curl -fsS -X POST "$URL/api/v1/diagnostics/export" -H "$CSRF" \
    -H 'Content-Type: application/json' --data '{}')
printf '%s' "$bundle" | jq -e \
    '.ok and .data.format == "libreecho-diagnostic-bundle" and
     .data.runtime != null and .data.manifest != null and
     .data.audio.available == false and .data.bluetooth.available == false and
     (.data.network | has("ssid") | not) and (.data.network | has("ip") | not)' >/dev/null
# Boot-slot countdown evidence must survive into the bundle: it is the only
# record that explains a device which stopped booting.
printf '%s' "$bundle" | jq -e \
    '.data.boot_control.available == true and
     .data.boot_control.state == "failed" and
     .data.boot_control.mode == "first-boot" and
     .data.boot_control.confirmed == false and
     .data.boot_control.running_slot == "a" and
     .data.boot_control.running_slot_tries == 0 and
     .data.boot_control.boot_count == 4 and
     .data.boot_control.last_check == "startup-ready-marker-missing" and
     (.data.boot_control.history | length) == 2' >/dev/null
# Release identity must describe the running device: the owner-selected
# channel (not a hard-coded "stable") and the slot boot-health reports running.
printf '%s' "$bundle" | jq -e \
    '.data.release_identity.channel == "dev" and
     .data.release_identity.active_slot == "a"' >/dev/null
printf '%s' "$bundle" | jq -e \
    '.data.manifest.sections | index("boot_control") != null' >/dev/null
printf '%s\n' 'diagnostic export: missing Linux adapters and malformed-status fixture remain non-fatal'
