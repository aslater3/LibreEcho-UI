#!/bin/sh
set -eu

grep -q '<div class="auth-brand"' web/login.html
grep -Eq '\.auth-brand em\{[^}]*font-style:normal' web/css/auth.css
