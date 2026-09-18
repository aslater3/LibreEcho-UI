#!/bin/sh
# The OTA upload limit is the device's, not a constant.
#
# GET /api/v1/system/update reports two numbers: max_upload_ceiling_bytes, the
# fixed limit the HTTP layer refuses above, and max_upload_bytes, which is that
# ceiling capped by what the filesystem the package is staged on can actually
# hold. A client that reads only the ceiling will offer an upload the device
# cannot store.
#
# This test drives the real daemon with LIBREECHO_UPDATE_STAGE pointed at a
# directory it controls, and checks the reported limit against df. It covers
# the clamp-to-ceiling case, which is what any normal filesystem produces. The
# low-space branch needs a nearly-full filesystem and is not reproducible in a
# plain CI checkout; it is left to review of the arithmetic in api.c.
set -eu

URL=${LIBREECHO_UPDATE_SIZE_URL:-http://127.0.0.1:18086}
PORT=${URL##*:}
BIN=${LIBREECHO_TEST_WEB:-./build/libreecho-web}
CEILING=33554432
MARGIN=4194304

stage=$(mktemp -d)
work=$(mktemp -d)
pid=0
cleanup(){ [ "$pid" -gt 1 ] && { kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }
           rm -rf "$stage" "$work"; }
trap cleanup EXIT INT TERM

LIBREECHO_UPDATE_STAGE="$stage" "$BIN" --backend mock \
    --config "$work/config.json" --listen "127.0.0.1:$PORT" \
    --allow-insecure-lan >"$work/daemon.log" 2>&1 &
pid=$!

i=0
while ! curl -fsS -m 1 "$URL/api/v1/config" >/dev/null 2>&1; do
    i=$((i + 1))
    [ "$i" -ge 50 ] && { echo "daemon did not start" >&2; cat "$work/daemon.log" >&2; exit 1; }
    sleep 0.1
done

body=$(curl -fsS "$URL/api/v1/system/update")
ceiling=$(printf '%s' "$body" | jq -r '.data.max_upload_ceiling_bytes')
limit=$(printf '%s' "$body" | jq -r '.data.max_upload_bytes')

[ "$ceiling" = "$CEILING" ] || { echo "ceiling is $ceiling, expected $CEILING" >&2; exit 1; }
[ "$limit" -ge 0 ] || { echo "limit $limit is negative" >&2; exit 1; }
[ "$limit" -le "$ceiling" ] || {
    echo "limit $limit exceeds the ceiling $ceiling the HTTP layer enforces" >&2; exit 1; }

free_kb=$(df -Pk "$stage" | awk 'NR==2{print $4}')
free_bytes=$((free_kb * 1024))
if [ "$((free_bytes - MARGIN))" -ge "$CEILING" ]; then
    # Room to spare, so the ceiling is the binding limit and the number is exact.
    [ "$limit" = "$CEILING" ] || {
        echo "with $free_bytes bytes free the limit should be the ceiling, got $limit" >&2
        exit 1; }
else
    # Tight filesystem: the limit must have come down, and must not be the ceiling.
    [ "$limit" -lt "$CEILING" ] || {
        echo "only $free_bytes bytes free but the limit is still the ceiling" >&2; exit 1; }
fi

echo "OTA upload limit contract: ok (limit $limit of ceiling $ceiling)"
