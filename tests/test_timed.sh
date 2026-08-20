#!/bin/sh
set -eu

root=$(mktemp -d "${TMPDIR:-/tmp}/libreecho-timed-test.XXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM

cat >"$root/ntp.conf" <<'EOF'
server time.cloudflare.com
server time.nist.gov
server ntp1.npl.co.uk
server ntp2.npl.co.uk
EOF
: >"$root/network-ready"
: >"$root/rtc0"

cat >"$root/hwclock" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$LIBREECHO_TEST_HWCLOCK_LOG"
EOF
chmod 0755 "$root/hwclock"

cat >"$root/ntpd" <<'EOF'
#!/bin/sh
peers=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -p) peers="${peers}${peers:+,}$2"; shift 2 ;;
        *) shift ;;
    esac
done
[ "$peers" = "time.cloudflare.com,time.nist.gov,ntp1.npl.co.uk,ntp2.npl.co.uk" ]
exit 0
EOF
chmod 0755 "$root/ntpd"

export LIBREECHO_TEST_HWCLOCK_LOG="$root/hwclock.log"
./build/libreecho-timed \
    --one-shot \
    --ntpd "$root/ntpd" \
    --hwclock "$root/hwclock" \
    --config "$root/ntp.conf" \
    --persistent-config "$root/missing-persistent.conf" \
    --status "$root/time.status" \
    --rtc-device "$root/rtc0" \
    --rtc-sysfs-dev "$root/missing-sysfs-dev" \
    --network-ready-file "$root/network-ready" \
    --retry-seconds 1

grep -qx 'state=synchronized' "$root/time.status"
grep -qx 'source=ntp' "$root/time.status"
grep -qx 'synchronized=1' "$root/time.status"
grep -qx 'rtc_available=1' "$root/time.status"
grep -qx 'rtc_persisted=1' "$root/time.status"
grep -qx 'config_source=image' "$root/time.status"
grep -qx 'servers=time.cloudflare.com,time.nist.gov,ntp1.npl.co.uk,ntp2.npl.co.uk' "$root/time.status"
grep -qx -- "-u -w -f $root/rtc0" "$root/hwclock.log"

cat >"$root/ntpd-failure" <<'EOF'
#!/bin/sh
printf '%s\n' 'simulated DNS failure' >&2
exit 7
EOF
chmod 0755 "$root/ntpd-failure"
if ./build/libreecho-timed \
    --one-shot \
    --ntpd "$root/ntpd-failure" \
    --config "$root/ntp.conf" \
    --persistent-config "$root/missing-persistent.conf" \
    --status "$root/failure.status" \
    --rtc-device "$root/rtc0" \
    --rtc-sysfs-dev "$root/missing-sysfs-dev" \
    --network-ready-file "$root/network-ready" >/dev/null 2>&1; then
    echo "failed ntpd was accepted" >&2
    exit 1
fi
grep -qx 'state=failed' "$root/failure.status"
grep -qx 'ntpd_exit_status=7' "$root/failure.status"
grep -qx 'last_error=simulated DNS failure ' "$root/failure.status"

cat >"$root/persistent.conf" <<'EOF'
server ntp.example.test
EOF
cat >"$root/ntpd-persistent" <<'EOF'
#!/bin/sh
peer=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -p) peer=$2; shift 2 ;;
        *) shift ;;
    esac
done
[ "$peer" = "ntp.example.test" ]
exit 0
EOF
chmod 0755 "$root/ntpd-persistent"
./build/libreecho-timed \
    --one-shot \
    --ntpd "$root/ntpd-persistent" \
    --hwclock "$root/hwclock" \
    --config "$root/ntp.conf" \
    --persistent-config "$root/persistent.conf" \
    --status "$root/persistent.status" \
    --rtc-device "$root/rtc0" \
    --rtc-sysfs-dev "$root/missing-sysfs-dev" \
    --network-ready-file "$root/network-ready"
grep -qx 'config_source=persistent' "$root/persistent.status"
grep -qx 'servers=ntp.example.test' "$root/persistent.status"

cat >"$root/invalid.conf" <<'EOF'
server time.example.test;touch-pwned
EOF
if ./build/libreecho-timed \
    --one-shot \
    --ntpd "$root/ntpd" \
    --config "$root/invalid.conf" \
    --persistent-config "$root/missing-persistent.conf" \
    --status "$root/invalid.status" \
    --rtc-device "$root/rtc0" \
    --network-ready-file "$root/network-ready" >/dev/null 2>&1; then
    echo "invalid NTP peer was accepted" >&2
    exit 1
fi
[ ! -e "$root/touch-pwned" ]

echo "timed: NTP selection, synchronization result, RTC persistence and validation: ok"
