#!/bin/sh
set -eu

# This audit is read-only: it excludes /audio, /audio/test, /wake-word/test,
# /led/test, all PUT/POST/DELETE actions, Wi-Fi connect/disconnect, restore,
# and power operations. It supports the normal authenticated device config.
: "${LIBREECHO_LIVE_URL:?set LIBREECHO_LIVE_URL to the explicit device URL}"
URL=$LIBREECHO_LIVE_URL
TMP_BASE=${TMPDIR:-/tmp}/libreecho-live-audit.$$
AUTH_HEADER=
TOKEN=
CSRF=

cleanup() {
    rm -f "$TMP_BASE".*
}
trap cleanup EXIT HUP INT TERM
umask 077

request_json() {
    if [ -n "$AUTH_HEADER" ]; then
        curl -fsS -H "$AUTH_HEADER" "$URL$1"
    else
        curl -fsS "$URL$1"
    fi
}

request_status() {
    if [ -n "$AUTH_HEADER" ]; then
        curl -sS -o "$TMP_BASE.response" -w '%{http_code}' \
            -H "$AUTH_HEADER" "$URL$1"
    else
        curl -sS -o "$TMP_BASE.response" -w '%{http_code}' "$URL$1"
    fi
}

config=$(curl -fsS "$URL/api/v1/config")
CSRF_VALUE=$(printf '%s' "$config" | jq -er '.data.csrf_token | select(test("^[0-9a-f]{64}$"))')
CSRF="X-LibreEcho-CSRF: $CSRF_VALUE"
AUTH_MODE=$(printf '%s' "$config" | jq -r '.data.authentication')

# Keep this anonymous protection check separate from the authenticated audit.
if [ "$AUTH_MODE" != "none" ] && [ "$AUTH_MODE" != "" ]; then
    anonymous_status=$(curl -sS -o "$TMP_BASE.anonymous" -w '%{http_code}' \
        "$URL/api/v1/status")
    [ "$anonymous_status" = 401 ] || {
        echo "expected anonymous protected request to return 401, got $anonymous_status" >&2
        exit 1
    }
fi

case "$AUTH_MODE" in
    none|"") ;;
    users)
        : "${LIBREECHO_LIVE_USERNAME:?set LIBREECHO_LIVE_USERNAME for authenticated live audit}"
        : "${LIBREECHO_LIVE_PASSWORD:?set LIBREECHO_LIVE_PASSWORD for authenticated live audit}"
        session=$(jq -cn --arg username "$LIBREECHO_LIVE_USERNAME" \
            --arg password "$LIBREECHO_LIVE_PASSWORD" \
            '{username:$username,password:$password}' |
            curl -fsS -X POST "$URL/api/v1/auth/login" \
                -H "$CSRF" -H 'Content-Type: application/json' --data-binary @-)
        TOKEN=$(printf '%s' "$session" | jq -er '.data.token | select(test("^[0-9a-f]{64}$"))')
        AUTH_HEADER="Authorization: Bearer $TOKEN"
        ;;
    bearer-token)
        : "${LIBREECHO_LIVE_TOKEN:?set LIBREECHO_LIVE_TOKEN for authenticated live audit}"
        TOKEN=$LIBREECHO_LIVE_TOKEN
        AUTH_HEADER="Authorization: Bearer $TOKEN"
        ;;
    *)
        echo "unsupported live authentication mode: $AUTH_MODE" >&2
        exit 1
        ;;
esac

json() { request_json "$1"; }
status() { request_status "$1"; }

json /api/v1 | jq -e '.ok and .data.name == "LibreEcho API" and .data.version == "v1"' >/dev/null
printf '%s' "$config" | jq -e '.ok and (.data.csrf_token | test("^[0-9a-f]{64}$")) and (.data.authentication | type == "string") and (.data.bind_policy | type == "string")' >/dev/null
status_json=$(json /api/v1/status)
printf '%s' "$status_json" | jq -e '.ok and .data.cpus.count >= 1 and (.data.cpus.cores | length) == .data.cpus.count and all(.data.cpus.cores[]; (.id | type) == "number" and (.online | type) == "boolean" and (.utilization_percent >= 0 and .utilization_percent <= 100) and (.frequency_khz >= 0)) and (.data.temperature_c | type) == "number" and (.data.storage_available | type) == "boolean" and (.data.storage_state | type) == "string" and (if .data.storage_available then (.data.storage_percent >= 0 and .data.storage_percent <= 100 and .data.storage_used_mb >= 0 and .data.storage_total_mb > 0) else (.data.storage_percent == null and .data.storage_used_mb == null and ((.data.storage_total_mb == null) or (.data.storage_total_mb >= 0))) end)' >/dev/null
device=$(json /api/v1/device)
printf '%s' "$device" | jq -e '.ok and (.data.name | length) > 0 and (.data.kernel | length) > 0' >/dev/null
network=$(json /api/v1/network)
printf '%s' "$network" | jq -e '.ok and (.data.state | type) == "string" and (.data.connectivity | type) == "string" and (.data.recovery_stage | type) == "string" and ((.data.gateway_reachable | type) == "boolean" or .data.gateway_reachable == null) and (.data.liveness_failures | type) == "number" and (.data.ssid | type) == "string" and (.data.rssi_dbm | type) == "number" and (.data.ip | type) == "string" and (.data.api_lan | type) == "boolean" and (.data.api_lan_effective | type) == "boolean" and (.data.api_lan_forced | type) == "boolean"' >/dev/null
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

log_headers=$TMP_BASE.log.headers
log_stream=$TMP_BASE.log.stream
if [ -n "$AUTH_HEADER" ]; then
    curl -fsS -D "$log_headers" -H "$AUTH_HEADER" "$URL/api/v1/logs/stream" -o "$log_stream"
else
    curl -fsS -D "$log_headers" "$URL/api/v1/logs/stream" -o "$log_stream"
fi
grep -qi '^content-type: text/event-stream' "$log_headers"
grep -q '^event: logs$' "$log_stream"
events_headers=$TMP_BASE.events.headers
events=$TMP_BASE.events
if [ -n "$AUTH_HEADER" ]; then
    curl -fsS --max-time 3 -D "$events_headers" -H "$AUTH_HEADER" "$URL/api/v1/events" -o "$events"
else
    curl -fsS --max-time 3 -D "$events_headers" "$URL/api/v1/events" -o "$events"
fi
grep -qi '^content-type: text/event-stream' "$events_headers"
grep -q '^event: status$' "$events"

for route in /api/v1/status /api/v1/device /api/v1/config /api/v1/config/export /api/v1/led /api/v1/buttons /api/v1/network /api/v1/privacy /api/v1/integrations /api/v1/system /api/v1/logs /api/v1/diagnostics /openapi.json; do
    if printf '%s' "$(json "$route")" | grep -Eiq '(^|[^A-Za-z])(undefined|NaN)([^A-Za-z]|$)'; then
        echo "placeholder found in JSON response: $route" >&2
        exit 1
    fi
done
[ "$(status /api/v1/network/wifi/scan)" = 200 ]
wake_word_status=$(status /api/v1/wake-word)
[ "$wake_word_status" = 200 ] || [ "$wake_word_status" = 501 ]
index=$(curl -fsS "$URL/")
app_path=$(printf '%s' "$index" | sed -n 's|.*src="\(/js/app.js?rev=[0-9][0-9]*\)".*|\1|p' | head -1)
[ -n "$app_path" ]
printf '%s' "$index" | grep -q 'auth-dialog'
printf '%s' "$index" | grep -q 'assets/favicon.svg'
app=$(curl -fsS "$URL$app_path")
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
echo 'live read-only audit: PASS (authenticated read-only checks; audio and hardware actions not invoked)'
