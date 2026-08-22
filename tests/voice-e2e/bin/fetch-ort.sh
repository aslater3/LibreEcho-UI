#!/bin/bash
# Fetch the third-party pieces the harness links against: the onnxruntime
# release for each host arch, and the openWakeWord models.
#
# build.sh referenced this script but it did not exist, so the harness only
# ever built on a machine where vendor/ had been populated by hand. Everything
# here comes from an upstream release and is pinned by sha256, so a fresh
# checkout can bootstrap and cannot silently pick up a different build.
set -euo pipefail
H="$(cd "$(dirname "$0")/.." && pwd)"
ORT_VERSION=1.17.3
OWW_VERSION=v0.5.1

want() {  # want <sha256> <path>
  [ -f "$2" ] || return 1
  local have
  have=$(shasum -a 256 "$2" 2>/dev/null | cut -d' ' -f1) || return 1
  [ "$have" = "$1" ]
}
get() {   # get <url> <sha256> <path>
  if want "$2" "$3"; then echo "  ok       $(basename "$3")"; return 0; fi
  echo "  fetching $(basename "$3")"
  mkdir -p "$(dirname "$3")"
  curl -fsSL --retry 3 -o "$3.part" "$1"
  local have; have=$(shasum -a 256 "$3.part" | cut -d' ' -f1)
  if [ "$have" != "$2" ]; then
    rm -f "$3.part"
    echo "  SHA MISMATCH for $(basename "$3")" >&2
    echo "    expected $2" >&2
    echo "    got      $have" >&2
    return 1
  fi
  mv "$3.part" "$3"
}

echo "== openWakeWord models =="
OWW=https://github.com/dscripka/openWakeWord/releases/download/$OWW_VERSION
get "$OWW/alexa_v0.1.onnx"      6ff566a01d12670e8d9e3c59da32651db1575d17272a601b7f8a39283dfbae3e "$H/models/alexa_v0.1.onnx"
get "$OWW/embedding_model.onnx" 70d164290c1d095d1d4ee149bc5e00543250a7316b59f31d056cff7bd3075c1f "$H/models/embedding_model.onnx"
get "$OWW/melspectrogram.onnx"  ba2b0e0f8b7b875369a2c89cb13360ff53bac436f2895cced9f479fa65eb176f "$H/models/melspectrogram.onnx"

echo "== onnxruntime $ORT_VERSION =="
REL=https://github.com/microsoft/onnxruntime/releases/download/v$ORT_VERSION
get "$REL/onnxruntime-linux-aarch64-$ORT_VERSION.tgz" \
    9f801577bd99676d1d821022e52b1f4554f56339ae3606c7b5ff3155f443c921 \
    "$H/vendor/ort-linux-aarch64.tgz"
get "$REL/onnxruntime-linux-x64-$ORT_VERSION.tgz" \
    f2f11f9da1e3e19b22a8b378b9af57a58433f40e3db6a803e75c0ec0eba97a20 \
    "$H/vendor/ort-linux-x64.tgz"

for a in aarch64 x64; do
  d="$H/vendor/onnxruntime-linux-$a-$ORT_VERSION"
  [ -d "$d" ] || { echo "  unpacking $a"; tar -xzf "$H/vendor/ort-linux-$a.tgz" -C "$H/vendor"; }
done
echo "ready."
