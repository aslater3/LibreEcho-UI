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
grep -Fq 'LOG_MAX_BYTES=${LOG_MAX_BYTES:-2097152}' "$script"
grep -Fq 'tail -c "$LOG_KEEP_BYTES" "$LOGFILE"' "$script"
grep -Fq '"--configfile", ctx->config_path' src/adapter/airplayd.c
! grep -Fq '"-v"' src/adapter/airplayd.c
! grep -Fq '"-vvv"' src/adapter/airplayd.c

calls=$(grep -c '^[[:space:]]*mount_led_socket$' "$script")
[ "$calls" -eq 2 ]
echo 'AirPlay LED socket isolation bridge: ok'
