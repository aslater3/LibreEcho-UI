#!/bin/sh
set -eu
PORT=${LIBREECHO_CPU_TEST_PORT:-18083}
URL="http://127.0.0.1:$PORT"
ROOT=./build/test-cpu-sysfs
CFG=./build/test-cpu-config.json
rm -rf "$ROOT"
mkdir -p "$ROOT" "$ROOT"/cpu0/cpufreq "$ROOT"/cpu1/cpufreq "$ROOT"/cpu2/cpufreq "$ROOT"/cpu3/cpufreq
printf '0-3\n' >"$ROOT/online"
: >"$CFG"
LIBREECHO_CPU_SYSFS_ROOT="$ROOT" \
./build/libreecho-web --backend linux --config "$CFG" --web-root ./web \
    --listen "127.0.0.1:$PORT" >./build/test-cpu-online.log 2>&1 &
pid=$!
cleanup(){ kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; }
trap cleanup EXIT INT TERM
i=0
while ! curl -fsS "$URL/api/v1/status" >/tmp/libreecho-cpu-status.json 2>/dev/null; do
    i=$((i + 1))
    [ "$i" -lt 30 ] || { cat ./build/test-cpu-online.log; exit 1; }
    sleep 0.1
done
jq -e '.ok and .data.cpus.cores[0].online and .data.cpus.cores[1].online and .data.cpus.cores[2].online and .data.cpus.cores[3].online and ([.data.cpus.cores[] | select(.online)] | length) == 4' /tmp/libreecho-cpu-status.json >/dev/null
printf '%s\n' 'cpu aggregate mask: ok'
