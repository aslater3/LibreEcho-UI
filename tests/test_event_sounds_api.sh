#!/bin/sh
# Owner event sounds over HTTP (#410): upload, list, select, preview, delete,
# and rejection of invalid files. Runs its own mock server with a private
# sound directory so it never touches /data.
set -eu

PORT=${LIBREECHO_SOUNDS_TEST_PORT:-18141}
URL="http://127.0.0.1:$PORT"
dir=$(mktemp -d)
cleanup() {
    [ -n "${pid:-}" ] && kill "$pid" 2>/dev/null || true
    [ -n "${pid:-}" ] && wait "$pid" 2>/dev/null || true
    rm -rf "$dir"
}
trap cleanup EXIT INT TERM

python3 - "$dir" <<'PY'
import math, struct, sys, wave
d = sys.argv[1]
def make(name, rate, channels, seconds, width=2):
    w = wave.open(f"{d}/{name}", "wb")
    w.setnchannels(channels); w.setsampwidth(width); w.setframerate(rate)
    frames = int(rate * seconds)
    if width == 2:
        w.writeframes(b"".join(struct.pack("<" + "h" * channels, *([int(6000 * math.sin(2 * math.pi * 660 * i / rate))] * channels)) for i in range(frames)))
    else:
        w.writeframes(bytes([128]) * frames * channels)
    w.close()
make("chime.wav", 44100, 2, 0.4)
make("ring.wav", 48000, 1, 3.0)
make("long.wav", 48000, 1, 1.2)
make("eight.wav", 48000, 1, 0.2, width=1)
with open(f"{d}/big.bin", "wb") as f:
    f.write(b"RIFF" + b"\0" * (1048576 + 10))
PY

LIBREECHO_USER_SOUND_DIR="$dir/sounds" ./build/libreecho-web --backend mock --config "$dir/config.json" \
    --web-root ./web --listen "127.0.0.1:$PORT" --seed 42 >"$dir/server.log" 2>&1 &
pid=$!
i=0
while ! curl -fsS "$URL/api/v1/status" >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 50 ] || { cat "$dir/server.log"; exit 1; }; sleep 0.1; done
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
WAV='Content-Type: audio/wav'
JSON='Content-Type: application/json'

expect() { # expect <code> <curl args...>
    want=$1; shift
    code=$(curl -sS -o "$dir/out" -w '%{http_code}' "$@")
    [ "$code" = "$want" ] || { echo "FAIL: $* returned $code, expected $want"; cat "$dir/out"; exit 1; }
}

# --- nothing uploaded: every event uses its built-in sound ---------------
curl -fsS "$URL/api/v1/sounds" | jq -e '.ok and (.data.sounds | map(.id)) == ["wake","end","timer"]
    and all(.data.sounds[]; .selected == "default" and .uploaded == false)
    and .data.max_upload_bytes == 1048576' >/dev/null
echo "  sound list defaults to built-in sounds: ok"

# --- upload: a 44.1 kHz stereo WAV becomes the selected 48 kHz mono sound -
expect 200 -X PUT "$URL/api/v1/sounds/wake" -H "$CSRF" -H "$WAV" --data-binary "@$dir/chime.wav"
jq -e '.data.sounds[0] | .selected == "custom" and .uploaded and .duration_ms == 400' "$dir/out" >/dev/null
[ "$(wc -c < "$dir/sounds/sound-wake.raw")" -eq 38400 ] || { echo "FAIL: wake sound not stored as 48 kHz mono"; exit 1; }
# Larger than the 16 KiB API body limit, so this exercises the streamed path.
expect 200 -X PUT "$URL/api/v1/sounds/timer" -H "$CSRF" -H "$WAV" --data-binary "@$dir/ring.wav"
jq -e '.data.sounds[2] | .selected == "custom" and .duration_ms == 3000' "$dir/out" >/dev/null
echo "  WAV uploads are converted and selected: ok"

# --- invalid files are rejected and leave the stored sound alone ---------
before=$(cksum < "$dir/sounds/sound-wake.raw")
expect 413 -X PUT "$URL/api/v1/sounds/wake" -H "$CSRF" -H "$WAV" --data-binary "@$dir/long.wav"
jq -e '.error.code == "sound_too_long"' "$dir/out" >/dev/null
expect 400 -X PUT "$URL/api/v1/sounds/wake" -H "$CSRF" -H "$WAV" --data-binary "not a wav file"
jq -e '.error.code == "invalid_sound"' "$dir/out" >/dev/null
expect 400 -X PUT "$URL/api/v1/sounds/wake" -H "$CSRF" -H "$WAV" --data-binary "@$dir/eight.wav"
expect 413 -X PUT "$URL/api/v1/sounds/wake" -H "$CSRF" -H "$WAV" --data-binary "@$dir/big.bin"
[ "$(cksum < "$dir/sounds/sound-wake.raw")" = "$before" ] || { echo "FAIL: a rejected upload changed the stored sound"; exit 1; }
# The 1.2 s file is fine for the 2 s end chime: limits are per event.
expect 200 -X PUT "$URL/api/v1/sounds/end" -H "$CSRF" -H "$WAV" --data-binary "@$dir/long.wav"
echo "  invalid and over-long files rejected without touching the stored sound: ok"

# --- the upload route keeps the API's security and routing rules ---------
expect 403 -X PUT "$URL/api/v1/sounds/wake" -H "$WAV" --data-binary "@$dir/chime.wav"
expect 404 -X PUT "$URL/api/v1/sounds/doorbell" -H "$CSRF" -H "$WAV" --data-binary "@$dir/chime.wav"
expect 404 -X PUT "$URL/api/v1/sounds/wake/extra" -H "$CSRF" -H "$WAV" --data-binary "@$dir/chime.wav"
expect 405 -X POST "$URL/api/v1/sounds" -H "$CSRF"
expect 405 -X GET "$URL/api/v1/sounds/wake/select"
echo "  upload requires CSRF and a known event: ok"

# --- select switches between the owner's sound and the built-in one ------
expect 200 -X POST "$URL/api/v1/sounds/timer/select" -H "$CSRF" -H "$JSON" --data '{"selected":"default"}'
jq -e '.data.sounds[2] | .selected == "default" and .uploaded' "$dir/out" >/dev/null
[ ! -e "$dir/sounds/sound-timer.raw" ] && [ -s "$dir/sounds/sound-timer.raw.saved" ] || { echo "FAIL: deselect did not keep the sound aside"; exit 1; }
expect 200 -X POST "$URL/api/v1/sounds/timer/select" -H "$CSRF" -H "$JSON" --data '{"selected":"custom"}'
jq -e '.data.sounds[2].selected == "custom"' "$dir/out" >/dev/null
expect 400 -X POST "$URL/api/v1/sounds/timer/select" -H "$CSRF" -H "$JSON" --data '{"selected":"loud"}'
expect 400 -X POST "$URL/api/v1/sounds/timer/select" -H "$CSRF" -H "$JSON" --data '{"selected":"custom","selected":"default"}'
echo "  select keeps and restores the owner's sound: ok"

# --- preview plays an uploaded sound; nothing to preview is a conflict ----
expect 200 -X POST "$URL/api/v1/sounds/wake/preview" -H "$CSRF"
jq -e '.data.playing == true' "$dir/out" >/dev/null
expect 200 -X DELETE "$URL/api/v1/sounds/wake" -H "$CSRF"
jq -e '.data.sounds[0] | .selected == "default" and .uploaded == false' "$dir/out" >/dev/null
[ ! -e "$dir/sounds/sound-wake.raw" ] || { echo "FAIL: delete left the sound on disk"; exit 1; }
expect 409 -X POST "$URL/api/v1/sounds/wake/preview" -H "$CSRF"
expect 409 -X POST "$URL/api/v1/sounds/wake/select" -H "$CSRF" -H "$JSON" --data '{"selected":"custom"}'
echo "  preview, delete and nothing-uploaded conflicts: ok"
