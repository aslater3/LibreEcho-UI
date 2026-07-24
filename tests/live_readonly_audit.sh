#!/bin/sh
set -eu

# This audit deliberately excludes /audio, /audio/test, /wake-word/test,
# /led/test, all PUT/POST/DELETE actions, Wi-Fi connect/disconnect, restore,
# and power operations. It is safe to run while someone is listening.
URL=${LIBREECHO_LIVE_URL:-http://192.168.0.125:8080}
json() { curl -fsS "$URL$1"; }
status() { curl -sS -o /tmp/libreecho-live-audit.out -w '%{http_code}' "$URL$1"; }

json /api/v1 | jq -e '.ok and .data.name == "LibreEcho API" and .data.version == "v1"' >/dev/null
config=$(json /api/v1/config)
printf '%s' "$config" | jq -e '.ok and (.data.csrf_token | test("^[0-9a-f]{64}$")) and (.data.authentication | type == "string") and (.data.bind_policy | type == "string")' >/dev/null
status_json=$(json /api/v1/status)
printf '%s' "$status_json" | jq -e '.ok and .data.cpus.count >= 1 and (.data.cpus.cores | length) == .data.cpus.count and all(.data.cpus.cores[]; (.id | type) == "number" and (.online | type) == "boolean" and (.utilization_percent >= 0 and .utilization_percent <= 100) and (.frequency_khz >= 0)) and (.data.temperature_c | type) == "number" and (.data.storage_available | type) == "boolean" and (.data.storage_state | type) == "string" and (if .data.storage_available then (.data.storage_percent >= 0 and .data.storage_percent <= 100 and .data.storage_used_mb >= 0 and .data.storage_total_mb > 0) else (.data.storage_percent == null and .data.storage_used_mb == null and ((.data.storage_total_mb == null) or (.data.storage_total_mb >= 0))) end)' >/dev/null
device=$(json /api/v1/device)
printf '%s' "$device" | jq -e '.ok and (.data.name | length) > 0 and (.data.kernel | length) > 0' >/dev/null
network=$(json /api/v1/network)
printf '%s' "$network" | jq -e '.ok and (.data.state | type) == "string" and (.data.ssid | type) == "string" and (.data.rssi_dbm | type) == "number" and (.data.ip | type) == "string" and (.data.api_lan | type) == "boolean" and (.data.api_lan_effective | type) == "boolean" and (.data.api_lan_forced | type) == "boolean"' >/dev/null
if printf '%s' "$config" | jq -e '.data.bind_policy == "lan-development"' >/dev/null; then
    printf '%s' "$network" | jq -e '.data.api_lan_effective == true and .data.api_lan_forced == true' >/dev/null
fi
led=$(json /api/v1/led)
printf '%s' "$led" | jq -e '.ok and (.data.animation_active | type) == "boolean" and (.data.animation_profile | type) == "string" and (.data.brightness >= 0 and .data.brightness <= 100) and all([.data.colour.r,.data.colour.g,.data.colour.b][]; . >= 0 and . <= 255)' >/dev/null
exported=$(json /api/v1/config/export)
printf '%s' "$exported" | jq -e '.ok and (.data.schema_version | type) == "number" and (.data.partial | type) == "boolean" and (.data.unsupported | type) == "array"' >/dev/null
system=$(json /api/v1/system)
printf '%s' "$system" | jq -e '.ok and (.data.ntp | type) == "boolean" and (.data.ntp_state | type) == "string" and (.data.ntp_servers | type) == "string" and (.data.last_sync_epoch | type) == "number" and (.data.clock_valid | type) == "boolean" and (.data.clock_source | type) == "string" and (.data.rtc_available | type) == "boolean" and (.data.rtc_persisted | type) == "boolean"' >/dev/null
logs=$(json /api/v1/logs)
printf '%s' "$logs" | jq -e '.ok and (.data.entries | type) == "array" and .data.bounded == true and (.data.source | type) == "string" and any(.data.entries[]?; (.boot_seconds | type) == "number" and .boot_seconds >= 0)' >/dev/null
json /api/v1/diagnostics | jq -e '.ok and (.data.checks | length) >= 1 and all(.data.checks[]; (.name | type) == "string" and (.status | type) == "string")' >/dev/null

scan=$(json /api/v1/network/wifi/scan)
printf '%s' "$scan" | jq -e '.ok and (.data.networks | type) == "array" and all(.data.networks[]; (.ssid | type) == "string" and (.security | type) == "string" and (.signal | type) == "number" and .signal >= 0 and .signal <= 100)' >/dev/null

log_headers=/tmp/libreecho-live-log-stream.headers
log_stream=/tmp/libreecho-live-log-stream.out
curl -fsS -D "$log_headers" "$URL/api/v1/logs/stream" -o "$log_stream"
grep -qi '^content-type: text/event-stream' "$log_headers"
grep -q '^event: logs$' "$log_stream"
events_headers=/tmp/libreecho-live-events.headers
events=/tmp/libreecho-live-events.out
curl -fsS --max-time 3 -D "$events_headers" "$URL/api/v1/events" -o "$events"
grep -qi '^content-type: text/event-stream' "$events_headers"
grep -q '^event: status$' "$events"

for route in /api/v1/status /api/v1/device /api/v1/config /api/v1/config/export /api/v1/led /api/v1/buttons /api/v1/network /api/v1/privacy /api/v1/integrations /api/v1/system /api/v1/logs /api/v1/diagnostics /openapi.json; do
    if printf '%s' "$(json "$route")" | grep -Eiq '(^|[^A-Za-z])(undefined|NaN)([^A-Za-z]|$)'; then
        echo "placeholder found in JSON response: $route" >&2
        exit 1
    fi
done
[ "$(status /api/v1/network/wifi/scan)" = 200 ]
[ "$(status /api/v1/wake-word)" = 501 ]
index=$(curl -fsS "$URL/")
printf '%s' "$index" | grep -q 'app.js?rev=13'
printf '%s' "$index" | grep -q 'auth-dialog'
printf '%s' "$index" | grep -q 'assets/favicon.svg'
app=$(curl -fsS "$URL/js/app.js?rev=13")
printf '%s' "$app" | grep -q 'led-ring-view'
printf '%s' "$app" | grep -q 'clock_source'
printf '%s' "$app" | grep -q 'ntp_servers'
printf '%s' "$app" | grep -q 'storageValue'
printf '%s' "$app" | grep -q 'usage unavailable'
printf '%s' "$app" | grep -q "csrf:''"
printf '%s' "$app" | grep -q 'openAuthDialog'
swagger=$(curl -fsS "$URL/js/swagger.js")
printf '%s' "$swagger" | grep -q 'csrfToken'
printf '%s' "$swagger" | grep -q 'configReady'
! printf '%s' "$swagger" | grep -q "prompt('Enter the LibreEcho API token')"
! printf '%s\n%s' "$app" "$swagger" | grep -q 'libreecho-local'
mark=$(curl -fsS "$URL/assets/mark.svg")
printf '%s' "$mark" | grep -q 'linearGradient'
printf '%s' "$mark" | grep -q 'stroke-dasharray="122 48"'
printf '%s' "$mark" | grep -q 'M22 20v24h21'
echo 'live read-only audit: PASS (audio and hardware actions not invoked)'
