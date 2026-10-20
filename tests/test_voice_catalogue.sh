#!/bin/sh
# Voice catalogue (#63): GET /audio/voices lists what the speaking engine really offers.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${LIBREECHO_VOICES_TEST_PORT:-18094}
WPORT=$((PORT + 1000))
URL="http://127.0.0.1:$PORT"
BIN="$ROOT/build/libreecho-web"
TMP=$(mktemp -d)
pid=0; wpid=0
cleanup() { for p in $pid $wpid; do [ "$p" -gt 0 ] && kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM
python3 "$ROOT/tests/fake_wyoming_describe.py" "$WPORT" >"$TMP/wy.log" 2>&1 &
wpid=$!
i=0; while ! grep -q ready "$TMP/wy.log" 2>/dev/null; do i=$((i+1)); [ "$i" -lt 50 ] || exit 1; sleep 0.1; done
LIBREECHO_TTS_VOICES="$TMP/voices.json" "$BIN" --backend mock --config "$TMP/config.json" --web-root "$ROOT/web" --listen "127.0.0.1:$PORT" --seed 42 --dev-controls >"$TMP/server.log" 2>&1 &
pid=$!
i=0; while ! curl -fsS "$URL/api/v1/config" >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 50 ] || exit 1; sleep 0.1; done
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
pipeline() { curl -fsS -X PUT "$URL/api/v1/voice-pipeline" -H "$CSRF" -H 'Content-Type: application/json' --data "$1" >/dev/null; }
curl -fsS -X PUT "$URL/api/v1/privacy" -H "$CSRF" -H 'Content-Type: application/json' --data '{"local_only":false}' >/dev/null

# Local engine without a voices.json: the two voices every payload carries.
curl -fsS "$URL/api/v1/audio/voices" | jq -e '.data.engine == "sherpa" and .data.source == "local" and ([.data.voices[].id] == ["southern-female","northern-male"])' >/dev/null
# A payload voices.json (Wyoming info shape) is listed instead.
printf '{"tts":[{"name":"local","voices":[{"name":"southern-female","languages":["en_GB"]},{"name":"bright-female","languages":["en_US"],"description":"Bright"}]}]}' > "$TMP/voices.json"
curl -fsS "$URL/api/v1/audio/voices" | jq -e '[.data.voices[].id] == ["southern-female","bright-female"] and .data.voices[1].name == "Bright"' >/dev/null

# Wyoming: the server's own catalogue, larger than one 2 KiB Wyoming data block.
pipeline "{\"mode\":\"custom\",\"stt_wyoming_uri\":\"tcp://127.0.0.1:10300\",\"stt_model\":\"whisper-small\",\"tts_wyoming_uri\":\"tcp://127.0.0.1:$WPORT\",\"tts_voice\":\"en_US-hfc_female-medium\"}"
v=$(curl -fsS "$URL/api/v1/audio/voices")
printf '%s' "$v" | jq -e '.data.engine == "wyoming" and .data.reachable == true and (.data.voices | length) == 31 and .data.active == "en_US-hfc_female-medium"' >/dev/null
printf '%s' "$v" | jq -e '.data.voices[1].installed == false and .data.voices[0].name == "Voice 0 \"quoted\"" and .data.voices[30].id == "en_US-hfc_female-medium"' >/dev/null
# Unreachable server: reported, not an error.
pipeline '{"mode":"custom","stt_wyoming_uri":"tcp://127.0.0.1:10300","stt_model":"whisper-small","tts_wyoming_uri":"tcp://127.0.0.1:9","tts_voice":"x"}'
curl -fsS "$URL/api/v1/audio/voices" | jq -e '.data.reachable == false and .data.voices == []' >/dev/null
pipeline '{"mode":"local"}'
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST "$URL/api/v1/audio/voices" -H "$CSRF" -H 'Content-Type: application/json' --data '{}')
[ "$code" = 405 ]
echo 'voice catalogue: ok'
