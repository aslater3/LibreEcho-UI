#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18082}
USERS=${LIBREECHO_TEST_USERS:-./build/test-users}
CSRF="X-LibreEcho-CSRF: $(curl -fsS "$URL/api/v1/config" | jq -r '.data.csrf_token')"
config=$(curl -fsS "$URL/api/v1/config")
printf '%s' "$config" | jq -e '.data.authentication == "users" and (.data.csrf_token | test("^[0-9a-f]{64}$"))' >/dev/null
code=$(curl -sS -o /tmp/le-user-status.out -w '%{http_code}' "$URL/api/v1/status")
[ "$code" = 401 ]
code=$(curl -sS -o /tmp/le-user-bad-login.out -w '%{http_code}' -X POST "$URL/api/v1/auth/login" -H "$CSRF" -H 'Content-Type: application/json' --data '{"username":"test-user","password":"wrong-password"}')
[ "$code" = 401 ]
session=$(curl -fsS -X POST "$URL/api/v1/auth/login" -H "$CSRF" -H 'Content-Type: application/json' --data '{"username":"test-user","password":"test-password-123"}')
token=$(printf '%s' "$session" | jq -r '.data.token')
[ "${#token}" = 64 ]
printf '%s' "$session" | jq -e '.data.username == "test-user" and .data.expires_in > 0' >/dev/null
curl -fsS "$URL/api/v1/auth" -H "Authorization: Bearer $token" | jq -e '.data.authenticated == true and .data.username == "test-user"' >/dev/null
curl -fsS "$URL/api/v1/status" -H "Authorization: Bearer $token" | jq -e '.data.cpus.count >= 1' >/dev/null
curl -fsS -X POST "$URL/api/v1/auth/logout" -H "Authorization: Bearer $token" -H "$CSRF" >/dev/null
code=$(curl -sS -o /tmp/le-user-after-logout.out -w '%{http_code}' "$URL/api/v1/status" -H "Authorization: Bearer $token")
[ "$code" = 401 ]
for i in 1 2 3 4; do
    code=$(curl -sS -o /tmp/le-user-rate.out -w '%{http_code}' -X POST "$URL/api/v1/auth/login" -H "$CSRF" -H 'Content-Type: application/json' --data '{"username":"test-user","password":"wrong-password"}')
    [ "$code" = 401 ]
done
code=$(curl -sS -o /tmp/le-user-rate.out -w '%{http_code}' -X POST "$URL/api/v1/auth/login" -H "$CSRF" -H 'Content-Type: application/json' --data '{"username":"test-user","password":"wrong-password"}')
[ "$code" = 429 ]
code=$(curl -sS -o /tmp/le-user-blocked.out -w '%{http_code}' -X POST "$URL/api/v1/auth/login" -H "$CSRF" -H 'Content-Type: application/json' --data '{"username":"test-user","password":"test-password-123"}')
[ "$code" = 429 ]
echo 'users, sessions and login rate limit: ok'
