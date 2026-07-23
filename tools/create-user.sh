#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 USERNAME PASSWORD" >&2
    exit 2
fi

username=$1
password=$2
case "$username" in
    ''|*[!A-Za-z0-9._-]*) echo "Invalid username" >&2; exit 2 ;;
esac
password_length=$(printf '%s' "$password" | wc -c | tr -d ' ')
if [ "$password_length" -lt 8 ] || [ "$password_length" -gt 128 ]; then
    echo "Password must be 8-128 characters" >&2
    exit 2
fi
command -v od >/dev/null 2>&1 || { echo "od is required" >&2; exit 2; }
command -v sha256sum >/dev/null 2>&1 || { echo "sha256sum is required" >&2; exit 2; }

salt=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
digest=$(printf '%s:%s' "$salt" "$password" | sha256sum | awk '{print $1}')
printf '%s:sha256:%s:%s\n' "$username" "$salt" "$digest"
