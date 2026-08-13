#!/bin/sh
set -eu

root=$(mktemp -d "${TMPDIR:-/tmp}/libreecho-timed-timeout.XXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM
printf 'server time.example.test\n' >"$root/ntp.conf"
: >"$root/network-ready"
: >"$root/rtc0"
cat >"$root/ntpd-hang" <<'EOF'
#!/bin/sh
printf '%s\n' 'simulated transport timeout' >&2
sleep 30
EOF
chmod 0755 "$root/ntpd-hang"

start=$(date +%s)
if ./build/libreecho-timed \
    --one-shot \
    --ntpd "$root/ntpd-hang" \
    --config "$root/ntp.conf" \
    --persistent-config "$root/missing-persistent.conf" \
    --status "$root/time.status" \
    --rtc-device "$root/rtc0" \
    --rtc-sysfs-dev "$root/missing-sysfs-dev" \
    --network-ready-file "$root/network-ready" \
    --ntpd-timeout-seconds 1 >/dev/null 2>&1; then
    echo 'timed ntpd unexpectedly succeeded' >&2
    exit 1
fi
elapsed=$(( $(date +%s) - start ))
[ "$elapsed" -lt 5 ]
grep -qx 'state=failed' "$root/time.status"
grep -qx 'ntpd_exit_status=signal-15' "$root/time.status"
grep -qx 'last_error=ntpd timed out' "$root/time.status"
printf '%s\n' 'timed: ntpd timeout diagnostics: ok'
