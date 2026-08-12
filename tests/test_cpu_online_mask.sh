#!/bin/sh
set -eu
ROOT=./build/test-cpu-sysfs
CFG=./build/test-cpu-config.json
rm -rf "$ROOT"
mkdir -p "$ROOT" "$ROOT"/cpu0/cpufreq "$ROOT"/cpu1/cpufreq "$ROOT"/cpu2/cpufreq "$ROOT"/cpu3/cpufreq
printf '0-3\n' >"$ROOT/online"
: >"$CFG"

start_server(){
    port=$1
    log=./build/test-cpu-online-$port.log
    LIBREECHO_CPU_SYSFS_ROOT="$ROOT" \
    ./build/libreecho-web --backend linux --config "$CFG" --web-root ./web \
        --listen "127.0.0.1:$port" >"$log" 2>&1 &
    pid=$!
    i=0
    while :; do
        if curl -fsS "http://127.0.0.1:$port/api/v1/status" >/tmp/libreecho-cpu-status.json 2>/dev/null; then
            PORT=$port
            return 0
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            cat "$log" >&2
            return 1
        fi
        i=$((i + 1))
        if [ "$i" -ge 30 ]; then
            cat "$log" >&2
            return 1
        fi
        sleep 0.1
    done
}

cleanup(){ kill "${pid:-0}" 2>/dev/null || true; wait "${pid:-0}" 2>/dev/null || true; }
trap cleanup EXIT INT TERM
base=$((20000 + ($$ % 10000)))
started=0
for offset in $(seq 0 19); do
    if start_server $((base + offset)); then
        started=1
        break
    fi
done
[ "$started" = 1 ]
jq -e '.ok and .data.cpus.cores[0].online and .data.cpus.cores[1].online and .data.cpus.cores[2].online and .data.cpus.cores[3].online and ([.data.cpus.cores[] | select(.online)] | length) >= 4' /tmp/libreecho-cpu-status.json >/dev/null
printf '%s\n' 'cpu aggregate mask: ok'
