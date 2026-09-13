#!/bin/sh
# PR #245 review regressions, exercised against the live mock server:
#
#   1. Enabling Home Assistant from a local, local-only configuration must
#      clear the persisted local-only requirement in the same transition, so
#      the device never claims network processing is forbidden while
#      libreecho-wyomingd streams microphone audio to Home Assistant. The state
#      the privacy endpoint refuses to author must not be reachable by toggling
#      the integration either.
#   2. A direct selection of home-assistant through PUT /api/v1/voice-pipeline
#      must record the previous pipeline so a later disable restores it instead
#      of falling back to local.
#   3. The Home Assistant status document must expose the satellite readiness
#      field (custom endpoints are only probed in custom mode).
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CFG=${LIBREECHO_TEST_CONFIG:-./build/test-suite-config.json}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
# Preserve the mode this test found so later API tests see the same state.
initial_mode=$(curl -fsS "$URL/api/v1/voice-pipeline" | jq -r '.data.mode')

put() {
    curl -fsS -X PUT "$URL$1" -H "$CSRF" -H 'Content-Type: application/json' --data "$2"
}
status_of() {
    curl -sS -o /tmp/le-ha-transition.out -w '%{http_code}' \
        -X PUT "$URL$1" -H "$CSRF" -H 'Content-Type: application/json' --data "$2"
}

# --- 1. local + local-only -> enable Home Assistant clears the flag ---------
put /api/v1/voice-pipeline '{"mode":"local"}' >/dev/null
put /api/v1/privacy '{"local_only":true}' >/dev/null
curl -fsS "$URL/api/v1/privacy" | jq -e '.data.local_only == true' >/dev/null

put /api/v1/integrations/home-assistant '{"enabled":true}' >/dev/null
curl -fsS "$URL/api/v1/voice-pipeline" |
    jq -e '.ok and .data.mode == "home-assistant"' >/dev/null
curl -fsS "$URL/api/v1/privacy" | jq -e '.data.local_only == false' >/dev/null
jq -e '.voice_pipeline_mode == "home-assistant" and .privacy_local_only == false' \
    "$CFG" >/dev/null

# The privacy endpoint rejects recreating the contradictory state...
[ "$(status_of /api/v1/privacy '{"local_only":true}')" = 409 ]
# ...and disabling restores the recorded local pipeline.
put /api/v1/integrations/home-assistant '{"enabled":false}' >/dev/null
curl -fsS "$URL/api/v1/voice-pipeline" | jq -e '.data.mode == "local"' >/dev/null

# --- 2. direct custom -> home-assistant records the previous mode -----------
put /api/v1/voice-pipeline \
    '{"mode":"custom","stt_wyoming_uri":"tcp://127.0.0.1:10300","stt_model":"whisper-small","tts_wyoming_uri":"tcp://127.0.0.1:10200","tts_voice":"en_GB-alan-medium"}' \
    >/dev/null
put /api/v1/voice-pipeline '{"mode":"home-assistant"}' >/dev/null
curl -fsS "$URL/api/v1/voice-pipeline" |
    jq -e '.data.mode == "home-assistant"' >/dev/null
jq -e '.voice_pipeline_previous_mode == "custom"' "$CFG" >/dev/null

put /api/v1/integrations/home-assistant '{"enabled":false}' >/dev/null
curl -fsS "$URL/api/v1/voice-pipeline" | jq -e '.data.mode == "custom"' >/dev/null
jq -e '.voice_pipeline_mode == "custom"' "$CFG" >/dev/null

# --- 3. Home Assistant status exposes the satellite readiness field ---------
put /api/v1/integrations/home-assistant '{"enabled":true}' >/dev/null
curl -fsS "$URL/api/v1/voice-pipeline" | jq -e \
    '.ok and (.data.home_assistant.ready | type) == "boolean" and
     .data.stt.reachable == false and .data.tts.reachable == false' >/dev/null
put /api/v1/integrations/home-assistant '{"enabled":false}' >/dev/null


# --- 4. Non-Home-Assistant integration toggles are persisted (finding disproved)
# The review claimed PUT /api/v1/integrations/<id> only mutates memory for every
# id other than home-assistant, so the toggle reverts on restart. It does not:
# the shared HTTP server persists every successful integration PUT through
# api_persist_configuration() after the handler runs (src/http_server.c), so the
# stored mask changes here too. This pins that behaviour so a second write is
# never added inside after_integration_change (which would double-write and can
# replace the saved configuration's backup).
put /api/v1/integrations/mqtt '{"enabled":true}' >/dev/null
jq -e '(.integrations % 4) >= 2' "$CFG" >/dev/null
curl -fsS "$URL/api/v1/integrations" |
    jq -e '([.data.items[] | select(.id == "mqtt") | .enabled] | .[0]) == true' >/dev/null
put /api/v1/integrations/mqtt '{"enabled":false}' >/dev/null
jq -e '(.integrations % 4) < 2' "$CFG" >/dev/null

# Restore the mode this test found so later API tests start from the same state.
put /api/v1/voice-pipeline "{\"mode\":\"$initial_mode\"}" >/dev/null

printf '%s\n' 'voice pipeline Home Assistant transitions: ok'
