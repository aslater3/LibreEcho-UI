#!/bin/sh
set -eu

script=init/libreecho-airplayd.init
sh -n "$script"
grep -Fq 'source_socket=/run/libreecho/led.sock' "$script"
grep -Fq 'target_socket=$RUNTIME_ROOT/run/libreecho/led.sock' "$script"
grep -Fq 'mount --bind "$source_socket" "$target_socket"' "$script"
grep -Fq 'umount "$RUNTIME_ROOT/run/libreecho/led.sock"' "$script"
grep -Fq 'CONFIG=${CONFIG:-/data/libreecho/config/web-config.json}' "$script"
grep -Fq 'airplay_enabled_at_boot=1' "$script"
grep -Fq 'persistent AirPlay disable' "$script"
grep -Fq 'ARGS="$ARGS --enable-on-start"' "$script"
grep -Fq 'integrations & 16' "$script"
grep -Fq 'LOG_MAX_BYTES=${LOG_MAX_BYTES:-2097152}' "$script"
grep -Fq 'tail -c "$LOG_KEEP_BYTES" "$LOGFILE"' "$script"
# The shared mDNS supervisor owns the Avahi runtime, so this wrapper must not
# prepare one. It stages only its own AirPlay service definitions into the
# supervisor's services directory.
grep -Fq 'AVAHI_SERVICES_SOURCE=${AVAHI_SERVICES_SOURCE:-/data/libreecho/features/airplay2/avahi-services}' "$script"
if grep -Eq 'dbus-daemon|avahi-daemon|run/avahi-daemon|var/lib/avahi|libreecho-airplay-avahi|prepare_avahi_runtime' "$script"; then
    echo 'airplayd init must not own a private discovery stack' >&2
    exit 1
fi
grep -Fq 'stage_airplay_services' "$script"
grep -Fq 'MDNS_SERVICES_DIR' "$script"
grep -Fq '"--configfile", ctx->config_path' src/adapter/airplayd.c
! grep -Fq '"-v"' src/adapter/airplayd.c
! grep -Fq '"-vvv"' src/adapter/airplayd.c

# Fresh, pre-mounted, complete and partial runtime paths are executed by
# test_airplay_premounted_runtime.py in the same test runner. That test checks
# the LED bridge is reached, rather than counting duplicate source call sites.
echo 'AirPlay LED socket isolation bridge: ok'
