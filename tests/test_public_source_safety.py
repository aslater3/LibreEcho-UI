#!/usr/bin/env python3
"""Fail if tracked UI source contains private workstation or device identities."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
tracked = subprocess.run(
    ["git", "ls-files", "-z"],
    cwd=ROOT,
    check=True,
    capture_output=True,
).stdout.split(b"\0")

private_home = "/" + "home" + "/" + "andy" + "/"
device_prefix = "G2A0" + "RF"
for raw_path in tracked:
    if not raw_path:
        continue
    path = ROOT / raw_path.decode("utf-8", errors="surrogateescape")
    try:
        text = path.read_text(encoding="utf-8")
    except (UnicodeDecodeError, IsADirectoryError):
        continue
    assert private_home not in text, f"private workstation path in {path.relative_to(ROOT)}"
    assert device_prefix not in text, f"private device identity in {path.relative_to(ROOT)}"

for forbidden in (
    "docs/LOCAL_AI_INTEGRATION_AUDIT.md",
    "deploy/hotstage-local-ai-root.sh",
):
    assert not (ROOT / forbidden).exists(), f"internal-only file is tracked: {forbidden}"

print("public source safety: ok")
