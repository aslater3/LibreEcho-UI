#!/bin/sh
# Owner-staged wake model selection in init/libreecho-waked.init: a valid
# selection runs the stock detector against it, and every kind of bad
# selection, including one that fails to load, falls back to the stock model.
set -eu

T=$(mktemp -d /tmp/libreecho-waked-init-XXXXXX)
INIT=$(pwd)/init/libreecho-waked.init
cleanup() {
    if [ -s "$T/waked.pid" ]; then kill "$(cat "$T/waked.pid")" 2>/dev/null || true; fi
    rm -rf "$T"
}
trap cleanup EXIT

mkdir -p "$T/bin" "$T/root/usr/local/sbin" "$T/models"
STOCK=$T/root/usr/local/share/libreecho/openwakeword
model() {
    mkdir -p "$1"
    for f in melspectrogram.onnx embedding_model.onnx alexa_v0.1.onnx; do
        echo onnx >"$1/$f"
    done
}
model "$STOCK"
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
# Records its arguments; a model directory named bad-load fails like an
# unloadable model does, by exiting at startup.
cat >"$T/root/usr/local/sbin/libreecho-waked" <<EOF
#!/bin/sh
echo "\$*" >>"$T/args"
case "\$*" in *"/bad-load "*) exit 1 ;; esac
exec sleep 20
EOF
chmod +x "$T/bin/"* "$T/root/usr/local/sbin/libreecho-waked"
python3 -c 'import socket,sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$T/mic.sock"

run() {
    PATH="$T/bin:$PATH" PAYLOAD="$T/payload" RUNTIME_ROOT="$T/root" \
    PIDFILE="$T/waked.pid" LOGFILE="$T/waked.log" SOCKET="$T/wake.sock" \
    MIC_SOCKET="$T/mic.sock" REFERENCE_SOCKET="$T/ref.sock" \
    VAD_FLOOR_FILE="$T/none" AEC_FILE="$T/none" DUMP_FILE="$T/none" \
    WAKE_MODEL_FILE="$T/wake-model" WAKE_MODEL_BASE="$T/models" \
    WAKE_MODEL_SETTLE_SECONDS=1 timeout 20 sh "$INIT" "$1" 2>>"$T/stderr"
}

# expect <case> <model-dir> [<model-dir> ...]: each start's --model-dir, in order.
expect() {
    name=$1; shift
    : >"$T/args"
    run start
    count=0
    for dir in "$@"; do
        count=$((count + 1))
        line=$(sed -n "${count}p" "$T/args")
        case "$line" in
            *"--model-dir $dir "*) ;;
            *) echo "FAIL $name: start $count used: $line" >&2; cat "$T/stderr" >&2; exit 1 ;;
        esac
    done
    [ "$(wc -l <"$T/args")" -eq "$count" ] || {
        echo "FAIL $name: expected $count start(s)" >&2; cat "$T/args" >&2; exit 1; }
    run status >/dev/null || { echo "FAIL $name: waked not running" >&2; exit 1; }
    run stop
    echo "waked init: $name"
}

expect "no selection runs the stock model" "$STOCK"

model "$T/models/mine"
echo mine >"$T/wake-model"
expect "a staged model is selected" "$T/models/mine"

echo "../mine" >"$T/wake-model"
expect "a path-like model name falls back to stock" "$STOCK"

model "$T/models/partial"
rm "$T/models/partial/alexa_v0.1.onnx"
echo partial >"$T/wake-model"
expect "an incomplete model falls back to stock" "$STOCK"

model "$T/models/linked"
rm "$T/models/linked/alexa_v0.1.onnx"
ln -s "$STOCK/alexa_v0.1.onnx" "$T/models/linked/alexa_v0.1.onnx"
echo linked >"$T/wake-model"
expect "a symlinked model file falls back to stock" "$STOCK"

model "$T/models/bad-load"
echo bad-load >"$T/wake-model"
expect "a model that fails to load restarts on stock" "$T/models/bad-load" "$STOCK"
grep -q "owner-staged model failed to start; using stock model" "$T/stderr"
