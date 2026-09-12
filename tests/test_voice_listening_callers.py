#!/usr/bin/env python3
"""Keep Wyoming visual-only while the local pipeline retains its wake chirp."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]


def function(source: Path, name: str) -> str:
    text = source.read_text(encoding="utf-8")
    match = re.search(
        rf"static [^\n]*\b{name}\(.*?^}}\n",
        text,
        re.DOTALL | re.MULTILINE,
    )
    if not match:
        raise SystemExit(f"{name} not found in {source}")
    return match.group(0)


def main() -> None:
    wyoming_start = function(ROOT / "src/adapter/wyomingd.c", "start_stream")
    local_start = function(
        ROOT / "src/adapter/voice_pipeline.c", "start_recognition"
    )

    if "le_voice_listening_led_set(1)" not in wyoming_start:
        raise SystemExit("Wyoming capture must start with visual-only feedback")
    if "le_voice_listening_feedback_set" in wyoming_start:
        raise SystemExit("Wyoming capture must not request the wake chirp")
    if "le_voice_listening_feedback_set(1)" not in local_start:
        raise SystemExit("local capture must retain audible wake feedback")

    print("voice listening callers: Wyoming silent, local chirped: ok")


if __name__ == "__main__":
    main()
