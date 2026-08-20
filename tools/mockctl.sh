#!/bin/sh
set -eu
BASE=${LIBREECHO_URL:-http://127.0.0.1:8080}
action=${1:-}
value=${2:-}
csrf=$(curl -fsS "$BASE/api/v1/config" | jq -r '.data.csrf_token')
case "$action" in
 set-temperature|set-wifi|fail-next|set-update-progress) [ "$#" -eq 2 ] || { echo "usage: $0 $action VALUE" >&2; exit 2; } ;;
 trigger) [ "$#" -eq 2 ] || { echo "usage: $0 trigger wake-word" >&2; exit 2; } ;;
 reset) value="" ;;
 *) echo "usage: $0 {set-temperature N|set-wifi STATE|fail-next OP|trigger wake-word|set-update-progress N|reset}" >&2; exit 2 ;;
esac
curl -fsS -X POST "$BASE/api/v1/dev/mock" -H 'Content-Type: application/json' -H "X-LibreEcho-CSRF: $csrf" --data "{\"action\":\"$action\",\"value\":\"$value\"}"
printf '\n'
