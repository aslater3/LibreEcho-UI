#!/bin/sh
# Four-band owner equaliser and speaker boost through the HTTP API (mock
# backend): GET shape, PUT presets and bands, validation (nothing changes on a
# 400), and the configuration export/import round trip. Leaves the device
# flat with no boost and at the volume it found.
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
OUT=${TMPDIR:-/tmp}/le-owner-eq.out
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
put(){ curl -fsS -X PUT "$URL/api/v1/audio" -H "$CSRF" -H 'Content-Type: application/json' --data "$1"; }
code_of(){ curl -sS -o "$OUT" -w '%{http_code}' -X PUT "$URL/api/v1/audio" -H "$CSRF" -H 'Content-Type: application/json' --data "$1"; }
state(){ curl -fsS "$URL/api/v1/audio" | jq -c '.data | {volume, eq, boost_db}'; }
fail(){ echo "owner eq api: $*" >&2; exit 1; }
# The server is shared with later suites: put the volume back as it was found.
volume0=$(curl -fsS "$URL/api/v1/audio" | jq -r '.data.volume')

# GET: flat, no boost, exact shape.
curl -fsS "$URL/api/v1/audio" | jq -e '.ok and .data.eq == {"preset":"flat","bass":0,"low_mid":0,"high_mid":0,"treble":0} and .data.boost_db == 0' >/dev/null ||
    fail 'default GET shape'

# Every named preset applies its own four values.
for p in 'flat 0 0 0 0' 'bass_boost 5 1 0 0' 'treble_boost 0 0 2 5' 'vocal -2 1 4 1' \
         'loudness 4 -1 1 4' 'late_night -4 0 2 0' 'warm 3 1 -1 -3'; do
    set -- $p
    put "{\"eq\":{\"preset\":\"$1\"}}" | jq -e --arg p "$1" --argjson b "$2" --argjson lm "$3" --argjson hm "$4" --argjson t "$5" \
        '.data.eq == {"preset":$p,"bass":$b,"low_mid":$lm,"high_mid":$hm,"treble":$t}' >/dev/null || fail "preset $1"
done

# A subset of bands keeps the others; moving a band makes the preset custom.
put '{"eq":{"preset":"vocal"}}' >/dev/null
put '{"eq":{"high_mid":-0.5}}' | jq -e '.data.eq == {"preset":"custom","bass":-2,"low_mid":1,"high_mid":-0.5,"treble":1}' >/dev/null ||
    fail 'subset of bands'
put '{"eq":{"bass":6,"low_mid":-6,"high_mid":5.5,"treble":-5.50,"preset":"custom"}}' |
    jq -e '.data.eq == {"preset":"custom","bass":6,"low_mid":-6,"high_mid":5.5,"treble":-5.5}' >/dev/null || fail 'range ends and halves'
# A named preset with explicit bands that differ is shown as custom.
put '{"eq":{"preset":"warm","bass":0}}' | jq -e '.data.eq == {"preset":"custom","bass":0,"low_mid":1,"high_mid":-1,"treble":-3}' >/dev/null ||
    fail 'preset with an overriding band'
# All four at 0 is flat, whatever name was asked for.
put '{"eq":{"bass":0,"low_mid":0,"high_mid":0,"treble":0,"preset":"custom"}}' | jq -e '.data.eq.preset == "flat"' >/dev/null ||
    fail 'zero bands are flat'

# Boost.
for db in 3 6 0; do
    put "{\"boost_db\":$db}" | jq -e --argjson db "$db" '.data.boost_db == $db' >/dev/null || fail "boost $db"
done

# Validation: every bad request is a 400 and changes nothing (volume included).
put '{"volume":41,"eq":{"preset":"loudness"},"boost_db":3}' >/dev/null
before=$(state)
for bad in \
    '{"eq":{"bass":6.5}}' '{"eq":{"treble":-7}}' '{"eq":{"bass":1.25}}' '{"eq":{"bass":0.1}}' \
    '{"eq":{"bass":"1"}}' '{"eq":{"bass":true}}' '{"eq":{"bass":1e0}}' '{"eq":{"bass":null}}' \
    '{"eq":{"preset":"rock"}}' '{"eq":{"preset":"Flat"}}' '{"eq":{"preset":1}}' '{"eq":{"loud":1}}' \
    '{"eq":{"bass":1,"bass":2}}' '{"eq":[1]}' '{"eq":"vocal"}' '{"eq":{}, "eq":{}}' \
    '{"boost_db":4}' '{"boost_db":-3}' '{"boost_db":3.5}' '{"boost_db":"3"}' '{"boost_db":true}' \
    '{"volume":12,"eq":{"bass":9}}' '{"volume":12,"boost_db":9}' '{"eq":{"bass":2},"boost_db":2}' \
    '{"eq":{"preset":"vocal"},"boost_db":1}'; do
    code=$(code_of "$bad")
    [ "$code" = 400 ] || fail "accepted $bad ($code)"
    jq -e '.ok == false and .error.code == "invalid_request"' "$OUT" >/dev/null || fail "error envelope for $bad"
    [ "$(state)" = "$before" ] || fail "$bad changed state: $(state) (was $before)"
done
# An empty eq object is accepted and changes nothing.
put '{"eq":{}}' >/dev/null
[ "$(state)" = "$before" ] || fail 'empty eq object changed state'

# Export carries the keys; import restores them; a bad value refuses the import.
put '{"eq":{"preset":"custom","bass":2.5,"low_mid":-1,"high_mid":0,"treble":3},"boost_db":6}' >/dev/null
exported=$(curl -fsS "$URL/api/v1/config/export" | jq -c '.data')
printf '%s' "$exported" | jq -e '.eq_preset == "custom" and .eq_bass == 2.5 and .eq_low_mid == -1 and .eq_high_mid == 0 and .eq_treble == 3 and .boost_db == 6' >/dev/null ||
    fail 'export keys'
put '{"eq":{"preset":"flat"},"boost_db":0}' >/dev/null
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" -H 'Content-Type: application/json' --data "$exported" >/dev/null
curl -fsS "$URL/api/v1/audio" | jq -e '.data.eq == {"preset":"custom","bass":2.5,"low_mid":-1,"high_mid":0,"treble":3} and .data.boost_db == 6' >/dev/null ||
    fail 'import round trip'
put '{"eq":{"preset":"vocal"},"boost_db":3}' >/dev/null
before=$(state)
for edit in '.eq_bass = 7' '.eq_treble = 0.25' '.eq_preset = "rock"' '.boost_db = 5' '.boost_db = 3.5' '.eq_low_mid = "1"'; do
    bad=$(printf '%s' "$exported" | jq -c "$edit")
    code=$(curl -sS -o "$OUT" -w '%{http_code}' -X POST "$URL/api/v1/config/import" -H "$CSRF" -H 'Content-Type: application/json' --data "$bad")
    [ "$code" = 400 ] || fail "import accepted $edit ($code)"
    [ "$(state)" = "$before" ] || fail "import with $edit changed state"
done
# An older export without the keys imports and leaves the setting alone.
legacy=$(printf '%s' "$exported" | jq -c 'del(.eq_preset, .eq_bass, .eq_low_mid, .eq_high_mid, .eq_treble, .boost_db)')
curl -fsS -X POST "$URL/api/v1/config/import" -H "$CSRF" -H 'Content-Type: application/json' --data "$legacy" >/dev/null
curl -fsS "$URL/api/v1/audio" | jq -e '.data.eq.preset == "vocal" and .data.boost_db == 3' >/dev/null || fail 'legacy import'

put "{\"volume\":$volume0,\"eq\":{\"preset\":\"flat\"},\"boost_db\":0}" >/dev/null
[ "$(state)" = "{\"volume\":$volume0,\"eq\":{\"preset\":\"flat\",\"bass\":0,\"low_mid\":0,\"high_mid\":0,\"treble\":0},\"boost_db\":0}" ] || fail "not restored: $(state)"
echo 'owner eq api: ok'
