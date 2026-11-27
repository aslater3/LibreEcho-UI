#!/bin/sh
# Wake-path noise suppression in init/libreecho-waked.init: a hand-provisioned
# attenuation file is passed through, clamped at the daemon ceiling, and any
# unreadable value leaves the daemon default alone rather than costing the
# device its wake word.
set -eu

T=$(mktemp -d /tmp/libreecho-waked-init-noise-XXXXXX)
INIT=$(pwd)/init/libreecho-waked.init
cleanup() {
    if [ -s "$T/waked.pid" ]; then kill "$(cat "$T/waked.pid")" 2>/dev/null || true; fi
    rm -rf "$T"
}
trap cleanup EXIT

mkdir -p "$T/bin" "$T/root/usr/local/sbin" "$T/models"
STOCK=$T/root/usr/local/share/libreecho/openwakeword
mkdir -p "$STOCK"
for f in melspectrogram.onnx embedding_model.onnx alexa_v0.1.onnx; do
    echo onnx >"$STOCK/$f"
done
: >"$T/payload"
printf '#!/bin/sh\nexit 0\n' >"$T/bin/mount"
cp "$T/bin/mount" "$T/bin/umount"
cat >"$T/bin/start-stop-daemon" <<'EOF'
#!/bin/sh
mode=; pidfile=; daemon=
while [ "$#" -gt 0 ]; do
    case "$1" in
        -S) mode=start ;; -K) mode=stop ;;
        -p) pidfile=$2; shift ;; -x) daemon=$2; shift ;;
        -R) shift ;; --) shift; break ;;
    esac
    shift
done
if [ "$mode" = stop ]; then
    kill "$(cat "$pidfile")" 2>/dev/null || true
    exit 0
fi
"$daemon" "$@" </dev/null >/dev/null 2>&1 &
echo "$!" >"$pidfile"
EOF
cat >"$T/root/usr/local/sbin/libreecho-waked" <<EOF
#!/bin/sh
echo "\$*" >>"$T/args"
exec sleep 20
EOF
chmod +x "$T/bin/"* "$T/root/usr/local/sbin/libreecho-waked"
python3 -c 'import socket,sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$T/mic.sock"

run() {
    PATH="$T/bin:$PATH" PAYLOAD="$T/payload" RUNTIME_ROOT="$T/root" \
    PIDFILE="$T/waked.pid" LOGFILE="$T/waked.log" SOCKET="$T/wake.sock" \
    MIC_SOCKET="$T/mic.sock" REFERENCE_SOCKET="$T/ref.sock" \
    VAD_FLOOR_FILE="$T/none" AEC_FILE="$T/none" DUMP_FILE="$T/none" \
    NOISE_FILE="$T/noise" \
    WAKE_MODEL_FILE="$T/wake-model" WAKE_MODEL_BASE="$T/models" \
    WAKE_SECOND_FILE="$T/wake-second" \
    WAKE_MODEL_SETTLE_SECONDS=1 timeout 20 sh "$INIT" "$1" 2>>"$T/stderr"
}

# expect_arg <case> <expected substring> [<substring that must be absent>]
expect_arg() {
    name=$1; want=$2; absent=${3:-}
    : >"$T/args"
    run start
    # The daemon records its argv from the background process; give it a
    # moment rather than racing the scheduler.
    n=0
    while [ ! -s "$T/args" ] && [ "$n" -lt 40 ]; do
        sleep 0.05; n=$((n + 1))
    done
    line=$(sed -n 1p "$T/args")
    case "$line" in
        *"$want"*) ;;
        *) echo "FAIL $name: start used: '$line'" >&2
           cat "$T/stderr" >&2; cat "$T/args" >&2; exit 1 ;;
    esac
    if [ -n "$absent" ]; then
        case "$line" in
            *"$absent"*) echo "FAIL $name: unexpected $absent in: $line" >&2; exit 1 ;;
        esac
    fi
    run stop
    echo "waked init noise: $name"
}

echo 12 >"$T/noise"
expect_arg "an attenuation file is passed through" "--noise-suppression-db 12"

echo 0 >"$T/noise"
expect_arg "zero disables suppression" "--noise-suppression-db 0"

echo 99 >"$T/noise"
expect_arg "above the ceiling is clamped" "--noise-suppression-db 30"

echo junk >"$T/noise"
expect_arg "junk leaves the daemon default" "--vad-floor-rms" "--noise-suppression-db"
grep -q "unreadable noise suppression 'junk'" "$T/stderr"

rm -f "$T/noise"
expect_arg "an absent file leaves the daemon default" "--vad-floor-rms" "--noise-suppression-db"