#!/bin/sh
set -eu

manifest=$(mktemp)
trap 'rm -f "$manifest"' EXIT
sh tools/source-provenance.sh --json > "$manifest"
python3 - "$manifest" <<'PY'
import json
import sys
from pathlib import Path

manifest = json.loads(Path(sys.argv[1]).read_text())
assert manifest["schema"] == 1
assert isinstance(manifest["source_commit"], str) and manifest["source_commit"]
assert isinstance(manifest["source_dirty"], bool)
assert len(manifest["source_digest"]) == 64

makefile = Path("Makefile").read_text()
private_home = "/" + "home" + "/" + "andy" + "/"
api = Path("src/api.c").read_text()
build = Path("deploy/build-arm.sh").read_text()
push = Path("deploy/push-adb.sh").read_text()
assert "LE_SOURCE_COMMIT" in makefile and "LE_SOURCE_DIGEST" in makefile
assert "$(HOME)/workspace" not in makefile
assert private_home not in makefile
assert "ORT_PREFIX ?=" in makefile
assert "RE2_ARCHIVE ?=" in makefile
assert "$(ORT_BUILD)/_deps/onnx-build/libonnx.a" in makefile
assert "$(RE2_ARCHIVE)" in makefile
assert "/api/v1/provenance" in api
assert "refusing to build a dirty source tree" in build
assert "source-provenance.json" in push
print("source provenance contract: ok")
PY
make provenance >/dev/null
python3 -m json.tool build/source-provenance.json >/dev/null
