#!/bin/sh
# Repo hygiene: Python bytecode caches must never be tracked, and must stay ignored.
set -eu
cd "$(dirname "$0")/.."
fail=0
tracked=$(git ls-files | grep -E '(^|/)__pycache__/|\.pyc$' || true)
if [ -n "$tracked" ]; then
    echo "FAIL: tracked bytecode caches:" >&2
    echo "$tracked" >&2
    fail=1
fi
grep -qx '__pycache__/' .gitignore || { echo "FAIL: .gitignore lacks __pycache__/" >&2; fail=1; }
grep -qx '\*\.pyc' .gitignore || { echo "FAIL: .gitignore lacks *.pyc" >&2; fail=1; }
[ "$fail" -eq 0 ] || exit 1
echo "PASS: no tracked bytecode caches; .gitignore ignores them"
