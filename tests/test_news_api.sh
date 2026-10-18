#!/bin/sh
# LibreEcho News (#121): off by default; fetch only when enabled, lazily and
# rate-limited; strict validation keeps the last good copy; mirror then GitHub
# fallback; turning off removes the cached copy. A fake curl serves fixtures.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT=${LIBREECHO_NEWS_TEST_PORT:-18095}
URL="http://127.0.0.1:$PORT"
BIN="$ROOT/build/libreecho-web"
TMP=$(mktemp -d)
pid=0
cleanup() { [ "$pid" -gt 0 ] && kill "$pid" 2>/dev/null; wait 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT INT TERM
[ -x "$BIN" ] || { echo "missing $BIN" >&2; exit 1; }
mkdir -p "$TMP/update" "$TMP/feeds"
: > "$TMP/ca.pem"
cat > "$TMP/curl" <<'CURL'
#!/bin/sh
# Fake curl: log the arguments, then serve the fixture named after the host.
printf '%s\n' "$*" >> "$NEWS_FAKE_LOG"
out=; last=
while [ $# -gt 0 ]; do [ "$1" = -o ] && { out=$2; shift; }; last=$1; shift; done
host=${last#https://}; host=${host%%/*}
[ -f "$NEWS_FAKE_FEEDS/$host" ] || exit 22
cp "$NEWS_FAKE_FEEDS/$host" "$out"
CURL
chmod 755 "$TMP/curl"
good='{"schema":"libreecho-news-v1","generated":"2026-10-18T00:00:00Z","items":[{"id":"release-radar-puffin-v0.14.0","type":"release","date":"2026-10-16","title":"LibreEcho 0.14.0","summary":"Stable release","url":"https://github.com/aslater3/LibreEcho/releases/tag/radar-puffin-v0.14.0"},{"id":"progress-0.15","type":"progress","date":"2026-10-18","title":"0.15 <b>in progress</b>","summary":"Room setting and more","url":"https://libreecho.org/news/0.15"}]}'
printf '%s' "$good" > "$TMP/feeds/mirror.test"
export NEWS_FAKE_LOG="$TMP/curl.log" NEWS_FAKE_FEEDS="$TMP/feeds"
export LIBREECHO_NEWS_DIR="$TMP/update" LIBREECHO_NEWS_CURL="$TMP/curl" LIBREECHO_NEWS_CA="$TMP/ca.pem" \
       LIBREECHO_NEWS_SOURCES="https://mirror.test/news/news-v1.json https://github.test/releases/news-v1.json"
: > "$NEWS_FAKE_LOG"
"$BIN" --backend mock --config "$TMP/config.json" --web-root "$ROOT/web" --listen "127.0.0.1:$PORT" --seed 42 --dev-controls >"$TMP/server.log" 2>&1 &
pid=$!
i=0; while ! curl -fsS "$URL/api/v1/config" >/dev/null 2>&1; do i=$((i+1)); [ "$i" -lt 50 ] || { cat "$TMP/server.log"; exit 1; }; sleep 0.1; done
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
put() { curl -sS -o "$TMP/out" -w '%{http_code}' -X PUT "$URL/api/v1/news" -H "$CSRF" -H 'Content-Type: application/json' --data "$1"; }
refresh() { curl -sS -o "$TMP/out" -w '%{http_code}' -X POST "$URL/api/v1/news/refresh" -H "$CSRF" -H 'Content-Type: application/json' --data '{}'; }
wait_state() { i=0; while ! curl -fsS "$URL/api/v1/news" | jq -e ".data.state == \"$1\"" >/dev/null; do i=$((i+1)); [ "$i" -lt 50 ] || { curl -fsS "$URL/api/v1/news"; return 1; }; sleep 0.1; done; }

# Off by default, and nothing is fetched while off.
curl -fsS "$URL/api/v1/news" | jq -e '.data.enabled == false and .data.state == "disabled" and .data.items == []' >/dev/null
[ "$(refresh)" = 409 ]
[ ! -s "$NEWS_FAKE_LOG" ]
for body in '{}' '{"enabled":"yes"}' '{"enabled":true,"enabled":false}'; do [ "$(put "$body")" = 400 ]; done
code=$(curl -sS -o /dev/null -w '%{http_code}' -X PUT "$URL/api/v1/news" -H 'Content-Type: application/json' --data '{"enabled":true}'); [ "$code" = 403 ]

# Turning on fetches once, in the background, with the pinned transport.
[ "$(put '{"enabled":true}')" = 200 ]
jq -e '.data.enabled == true' "$TMP/out" >/dev/null
wait_state fresh
curl -fsS "$URL/api/v1/news" | jq -e '(.data.items | length) == 2 and .data.items[0].id == "progress-0.15" and .data.items[0].title == "0.15 <b>in progress</b>" and .data.last_error == ""' >/dev/null
grep -q -- '--proto =https' "$NEWS_FAKE_LOG" && grep -q -- '--max-filesize 32768' "$NEWS_FAKE_LOG" && grep -q -- '--cacert' "$NEWS_FAKE_LOG"
! grep -q '?' "$NEWS_FAKE_LOG"
fetches=$(wc -l < "$NEWS_FAKE_LOG")
jq -e '.news_enabled == true' "$TMP/config.json" >/dev/null
# Fresh: reading again does not fetch, and a manual refresh is rate-limited.
curl -fsS "$URL/api/v1/news" >/dev/null; curl -fsS "$URL/api/v1/news" >/dev/null
[ "$(wc -l < "$NEWS_FAKE_LOG")" = "$fetches" ]
[ "$(refresh)" = 429 ]

# An invalid document never replaces the last good copy.
printf '{"schema":"libreecho-news-v1","items":[{"id":"x","type":"release","date":"2026-10-18","title":"T","summary":"S","url":"https://evil.example/x"}]}' > "$TMP/feeds/mirror.test"
printf 'schema=1\nlast_attempt=1\nlast_success=1\nrefreshing_since=0\nresult=ok\n' > "$TMP/update/news-status"
wait_state stale
curl -fsS "$URL/api/v1/news" | tee "$TMP/dbg.json" | jq -e '(.data.items | length) == 2 and .data.last_error == "invalid"' >/dev/null || { cat "$TMP/dbg.json"; cat "$TMP/update/news-status"; exit 1; }

# The mirror failing falls back to GitHub release assets.
rm "$TMP/feeds/mirror.test"; printf '%s' "$good" > "$TMP/feeds/github.test"
printf 'schema=1\nlast_attempt=1\nlast_success=1\nrefreshing_since=0\nresult=ok\n' > "$TMP/update/news-status"
wait_state fresh
tail -2 "$NEWS_FAKE_LOG" | grep -q 'https://github.test/releases/news-v1.json'

# Turning off removes the cached copy and status.
[ "$(put '{"enabled":false}')" = 200 ]
jq -e '.data.state == "disabled" and .data.items == []' "$TMP/out" >/dev/null
[ ! -e "$TMP/update/news-cache.json" ] && [ ! -e "$TMP/update/news-status" ]
echo 'news api: ok'
