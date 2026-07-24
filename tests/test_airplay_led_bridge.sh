#!/bin/sh
set -eu

script=init/libreecho-airplayd.init
sh -n "$script"
grep -Fq 'source_socket=/run/libreecho/led.sock' "$script"
grep -Fq 'target_socket=$RUNTIME_ROOT/run/libreecho/led.sock' "$script"
grep -Fq 'mount --bind "$source_socket" "$target_socket"' "$script"
grep -Fq 'umount "$RUNTIME_ROOT/run/libreecho/led.sock"' "$script"
grep -Fq 'CONFIG=${CONFIG:-/data/libreecho/config/web-config.json}' "$script"
grep -Fq 'ARGS="$ARGS --enable-on-start"' "$script"
grep -Fq 'integrations & 16' "$script"

calls=$(grep -c '^[[:space:]]*mount_led_socket$' "$script")
[ "$calls" -eq 2 ]
echo 'AirPlay LED socket isolation bridge: ok'
