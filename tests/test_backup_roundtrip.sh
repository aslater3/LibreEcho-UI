#!/bin/sh
# Rootless fixture only: no host /etc, /data, credentials or network is visible.
set -eu
for command in bwrap python3; do
    command -v "$command" >/dev/null 2>&1 || {
        printf 'backup roundtrip: required command missing: %s\n' "$command" >&2
        exit 1
    }
done
[ -x "$PWD/build/libreecho-web" ] || {
    printf '%s\n' 'backup roundtrip: build/libreecho-web is required' >&2
    exit 1
}
ROOT=$(mktemp -d "${TMPDIR:-/tmp}/libreecho-backup-test.XXXXXX")
trap 'rm -rf "$ROOT"' EXIT
mkdir -p "$ROOT/etc" "$ROOT/data" "$ROOT/run" "$ROOT/out"
# Mount only executable/library trees from the host. All writable paths are
# disposable fixtures; /src is read-only and the network namespace is private.
set -- bwrap --die-with-parent --new-session --unshare-all --clearenv \
    --setenv PATH /usr/bin:/bin --setenv LC_ALL C --setenv TMPDIR /tmp \
    --ro-bind /usr /usr
for directory in /bin /lib /lib64; do
    [ ! -d "$directory" ] || set -- "$@" --ro-bind "$directory" "$directory"
done
"$@" --ro-bind "$PWD" /src --chdir /src \
    --bind "$ROOT/etc" /etc --bind "$ROOT/data" /data \
    --bind "$ROOT/run" /run --bind "$ROOT/out" /out \
    --proc /proc --dev /dev --tmpfs /tmp \
    /usr/bin/python3 -B tests/test_backup_roundtrip.py
