#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18083}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
config=$(curl -fsS "$URL/api/v1/config")
printf '%s' "$config" | jq -e '.data.authentication == "bootstrap-required" and .data.bootstrap_required == true' >/dev/null
curl -fsS "$URL/" | grep -q 'Create your first local account'
# Deep-linking to an SPA route while bootstrap is required must serve the setup
# page, not the app shell (whose API calls would then 401 with "setup is
# required"). Regression for the real-URL pathname routing change: the bootstrap
# fallback must cover every extensionless route, not just "/".
for route in /system /network /integrations; do
    curl -fsS "$URL$route" | grep -q 'Create your first local account'
done
# A real asset path that does not exist must still 404 (not fall back to setup).
code=$(curl -sS -o /dev/null -w '%{http_code}' "$URL/nope.js")
[ "$code" = 404 ]
code=$(curl -sS -o /tmp/le-bootstrap-bad.out -w '%{http_code}' \
    -X POST "$URL/api/v1/auth/bootstrap" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"username":"admin","password":"short","password_confirm":"short"}')
[ "$code" = 400 ]
session=$(curl -fsS -X POST "$URL/api/v1/auth/bootstrap" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"username":"admin","password":"test-password-123","password_confirm":"test-password-123"}')
printf '%s' "$session" | jq -e '.data.username == "admin" and (.data.token | test("^[0-9a-f]{64}$"))' >/dev/null
token=$(printf '%s' "$session" | jq -r '.data.token')
config=$(curl -fsS "$URL/api/v1/config")
printf '%s' "$config" | jq -e '.data.authentication == "users" and .data.bootstrap_required == false' >/dev/null
curl -fsS "$URL/api/v1/auth/users" -H "Authorization: Bearer $token" | jq -e '.data.users == [{"username":"admin"}]' >/dev/null
curl -fsS -X POST "$URL/api/v1/auth/users" -H "Authorization: Bearer $token" -H "$CSRF" \
    -H 'Content-Type: application/json' \
    --data '{"username":"operator","password":"operator-password-123","password_confirm":"operator-password-123"}' |
    jq -e '.ok and ((.data.users | length) == 2)' >/dev/null
curl -fsS -X DELETE "$URL/api/v1/auth/users/operator" -H "Authorization: Bearer $token" -H "$CSRF" |
    jq -e '.ok and (.data.users | length) == 1' >/dev/null
code=$(curl -sS -o /tmp/le-last-user-delete.out -w '%{http_code}' \
    -X DELETE "$URL/api/v1/auth/users/admin" -H "Authorization: Bearer $token" -H "$CSRF")
[ "$code" = 409 ]
printf '%s\n' 'bootstrap and user management: ok'
