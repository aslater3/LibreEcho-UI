#!/bin/sh
set -eu
URL=${LIBREECHO_TEST_URL:-http://127.0.0.1:18084}
login_headers=$(curl -fsS -D - -o /tmp/libreecho-login-page.html "$URL/login")
printf '%s\n' "$login_headers" | grep -q '200 OK'
grep -q 'id="login-form"' /tmp/libreecho-login-page.html
grep -q 'id="login-error"' /tmp/libreecho-login-page.html
grep -q 'id="login-version"' /tmp/libreecho-login-page.html
grep -q 'config.os_version' web/js/login.js
grep -q "location.replace('/login')" web/js/app.js
grep -q "redirect('/')" web/js/login.js
grep -q 'account-setup' web/initial-setup.html
printf '%s\n' 'auth navigation and page contracts: ok'
