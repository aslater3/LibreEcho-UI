#!/bin/sh
# The now-playing contract: /api/v1/playback reports which source is playing,
# the station and track it named, and what this device can actually do about
# it; /api/v1/playback/transport is that transport.
#
# This runs against the mock backend, so what it proves is the contract and the
# routing. Whether a real station sends a StreamTitle at all is a property of
# the station, and only a real stream can show it.
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
JSON='Content-Type: application/json'
post(){ curl -sS -o /tmp/le-transport.out -w '%{http_code}' -X POST "$URL$1" \
    -H "$CSRF" -H "$JSON" --data "$2"; }

# The transport block is always present, and pause is never offered.
curl -fsS "$URL/api/v1/playback" | jq -e \
    '.ok and .data.transport.pause == false and
     (.data.transport.reason | type == "string") and
     (.data.transport.reason | length > 0) and
     (.data.metadata | has("station"))' >/dev/null

# AirPlay is the mock default: the phone drives it, so neither control works
# and the reason says so rather than the button failing when pressed.
curl -fsS "$URL/api/v1/playback" | jq -e \
    '.data.source == "airplay2" and .data.transport.play == false and
     .data.transport.stop == false and
     (.data.transport.reason | test("AirPlay"))' >/dev/null

[ "$(post /api/v1/playback/transport '{"action":"pause"}')" = 501 ]
jq -e '.ok == false and (.error.message | test("cannot be paused"))' \
    /tmp/le-transport.out >/dev/null
[ "$(post /api/v1/playback/transport '{"action":"skip"}')" = 400 ]
[ "$(post /api/v1/playback/transport '{}')" = 400 ]
[ "$(post /api/v1/playback/transport '{"action":"stop"}')" = 409 ]
[ "$(post /api/v1/playback/transport '{"action":"play"}')" = 409 ]

# A state-changing call still needs the CSRF header.
code=$(curl -sS -o /dev/null -w '%{http_code}' -X POST \
    "$URL/api/v1/playback/transport" -H "$JSON" --data '{"action":"stop"}')
[ "$code" = 403 ]
# and the endpoints declare their methods.
code=$(curl -sS -o /dev/null -w '%{http_code}' "$URL/api/v1/playback/transport")
[ "$code" = 405 ]
code=$(curl -sS -o /dev/null -w '%{http_code}' -X DELETE "$URL/api/v1/playback" \
    -H "$CSRF")
[ "$code" = 405 ]

stations='{"station_count":1,"station_0_word":"groove","station_0_name":"Groove Salad","station_0_url":"http://ice1.somafm.com/groovesalad-128-mp3","station_0_enabled":true}'
curl -fsS -X PUT "$URL/api/v1/integrations/radio" -H "$CSRF" -H "$JSON" \
    --data "$stations" >/dev/null
curl -fsS -X POST "$URL/api/v1/integrations/radio/play" -H "$CSRF" -H "$JSON" \
    --data '{"word":"groove"}' >/dev/null

# Unsupported verbs on the radio action and configuration routes are explicit
# 405s, rather than falling through to a misleading 404.
for method in GET PUT DELETE PATCH; do
    code=$(curl -sS -o /tmp/le-radio-play-method.out -w '%{http_code}' \
        -X "$method" "$URL/api/v1/integrations/radio/play" \
        -H "$CSRF" -H "$JSON" --data '{}')
    [ "$code" = 405 ]
    code=$(curl -sS -o /tmp/le-radio-stop-method.out -w '%{http_code}' \
        -X "$method" "$URL/api/v1/integrations/radio/stop" \
        -H "$CSRF" -H "$JSON" --data '{}')
    [ "$code" = 405 ]
done
for method in POST DELETE PATCH; do
    code=$(curl -sS -o /tmp/le-radio-config-method.out -w '%{http_code}' \
        -X "$method" "$URL/api/v1/integrations/radio" \
        -H "$CSRF" -H "$JSON" --data '{}')
    [ "$code" = 405 ]
done

# The card must be able to say which station and which track, not "media audio".
# The stored name wins over the stream's own icy-name, because it is the name
# the user chose.
curl -fsS "$URL/api/v1/playback" | jq -e \
    '.ok and .data.state == "playing" and .data.source == "radio" and
     .data.buses.media == true and
     .data.metadata.available == true and
     .data.metadata.station == "Groove Salad" and
     .data.metadata.title == "Simulated Artist - Simulated Track" and
     .data.metadata.artist == null and .data.metadata.album == null and
     .data.transport.stop == true and .data.transport.play == false and
     .data.transport.pause == false' >/dev/null

[ "$(post /api/v1/playback/transport '{"action":"play"}')" = 409 ]

# Stopping answers with the playback document read after the action, so a
# client can re-render from it without a second request.
[ "$(post /api/v1/playback/transport '{"action":"stop"}')" = 200 ]
jq -e '.ok and .data.state == "idle" and .data.metadata.title == null and
       .data.metadata.station == null and
       .data.transport.stop == false and .data.transport.play == true' \
    /tmp/le-transport.out >/dev/null

# "play" starts the last station again. It is a fresh connection, not a resume.
[ "$(post /api/v1/playback/transport '{"action":"play"}')" = 200 ]
jq -e '.ok and .data.source == "radio" and .data.transport.stop == true' \
    /tmp/le-transport.out >/dev/null
[ "$(post /api/v1/playback/transport '{"action":"stop"}')" = 200 ]

# A station switched off is not started again behind the user's back.
off='{"station_count":1,"station_0_word":"groove","station_0_name":"Groove Salad","station_0_url":"http://ice1.somafm.com/groovesalad-128-mp3","station_0_enabled":false}'
curl -fsS -X PUT "$URL/api/v1/integrations/radio" -H "$CSRF" -H "$JSON" \
    --data "$off" >/dev/null
curl -fsS "$URL/api/v1/playback" | jq -e '.data.transport.play == false' >/dev/null
[ "$(post /api/v1/playback/transport '{"action":"play"}')" = 409 ]

echo 'playback transport contract: ok'
