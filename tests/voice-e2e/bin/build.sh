#!/bin/bash
# Build the voice daemons for linux/amd64 inside a container, including a
# waked linked against the real openWakeWord ONNX engine.
#
# The repo only has an arm32 rule for waked-onnx (the device build cross-links
# a hand-built onnxruntime).  For host-side testing we link the same
# wake_engine_onnx.cpp against the upstream linux-x64 onnxruntime release, so
# the scoring code under test is the production source, not a reimplementation.
set -euo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
UI=${UI:-$(cd "$H/../.." && pwd)}
# arm64 by default: it is native on Apple Silicon, so it is faster AND
# behaves correctly.  Under emulated linux/amd64, signal delivery is broken
# (a caught SIGTERM kills the process outright), which makes every daemon look
# like it crashes on shutdown -- an artifact of the emulator, not the code.
PLATFORM=${PLATFORM:-linux/arm64}
case "$PLATFORM" in
  linux/arm64) ORT="$H/vendor/onnxruntime-linux-aarch64-1.17.3"; ARCHLIB=aarch64-linux-gnu ;;
  linux/amd64) ORT="$H/vendor/onnxruntime-linux-x64-1.17.3";     ARCHLIB=x86_64-linux-gnu ;;
  *) echo "unsupported PLATFORM=$PLATFORM" >&2; exit 2 ;;
esac
IMAGE=libreecho-voice-harness:4

[ -d "$ORT" ] || { echo "missing $ORT -- run fetch-ort.sh" >&2; exit 1; }

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "== building toolchain image =="
  docker build --platform "$PLATFORM" -t "$IMAGE" -f - "$H" >/dev/null <<'DOCKER'
FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential libspeexdsp-dev ca-certificates \
      python3 curl \
  && rm -rf /var/lib/apt/lists/*
DOCKER
fi

echo "== building daemons ($PLATFORM) =="
docker run --rm --platform "$PLATFORM" -e "ARCHLIB=$ARCHLIB" \
  -v "$UI":/work -v "$ORT":/ort:ro -v "$H":/harness:ro -w /work "$IMAGE" bash -euo pipefail -c '
B=build/harness
# Objects are architecture-specific and make cannot see that the platform
# changed, so a stale tree links binaries for the wrong ISA.  Start clean.
rm -rf $B
mkdir -p /tmp/ortinc/onnxruntime/core/session
cp /ort/include/*.h /tmp/ortinc/onnxruntime/core/session/
mkdir -p /tmp/speex/lib /tmp/speex/include
ln -sf /usr/lib/$ARCHLIB/libspeexdsp.a /tmp/speex/lib/libspeexdsp.a
ln -sf /usr/include/speex /tmp/speex/include/speex

# Daemons that build straight from the tree.
make -s BUILD=$B SPEEX_PREFIX=/tmp/speex \
  $B/libreecho-micd \
  $B/libreecho-sttd-wyoming \
  $B/libreecho-agentd \
  $B/mock-audio-adapter \
  $B/mock-llm-curl

# Harness-only helpers: the fake codec tools micd execs, and the curl stand-in.
gcc -O2 -Wall -Wextra -Wpedantic -std=c99 /harness/src/fake-tinycap.c -o $B/fake-tinycap
gcc -O2 -Wall -Wextra -Wpedantic -std=c99 /harness/src/fake-tinymix.c -o $B/fake-tinymix
gcc -O2 -Wall -Wextra -Wpedantic -std=c99 /harness/src/harness-curl.c -o $B/harness-curl

# waked + the ONNX wake engine, amd64.
CF="-D_POSIX_C_SOURCE=200809L -DLE_WAKE_ENGINE_ONNX -O2 -Wall -Wextra -Isrc -Isrc/adapter -I/tmp/speex/include"
for f in waked voice_aec voice_reference voice_dsp voice_stream wake_worker wake_led adapter_client adapter_server; do
  gcc $CF -std=c99 -c src/adapter/$f.c -o $B/$f.wake.o
done
gcc $CF -std=c99 -c src/log.c -o $B/log.wake.o
g++ -O2 -Wall -Isrc -Isrc/adapter -I/tmp/ortinc -c src/adapter/wake_engine_onnx.cpp -o $B/wake_engine_onnx.o
g++ -o $B/libreecho-waked-onnx $B/*.wake.o $B/wake_engine_onnx.o \
  /tmp/speex/lib/libspeexdsp.a -L/ort/lib -lonnxruntime -lpthread -lm -ldl
echo "-- built:"
ls -1 $B/libreecho-micd $B/libreecho-waked-onnx $B/libreecho-sttd-wyoming \
      $B/libreecho-agentd $B/mock-audio-adapter $B/fake-tinycap \
      $B/fake-tinymix $B/harness-curl
'
