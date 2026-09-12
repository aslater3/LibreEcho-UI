#!/usr/bin/env python3
"""Keep Wyoming visual-only while the local pipeline retains its wake chirp."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    wyoming = (ROOT / "src/adapter/wyomingd.c").read_text(encoding="utf-8")
    local = (ROOT / "src/adapter/voice_pipeline.c").read_text(encoding="utf-8")

    if "le_voice_listening_feedback_set" in wyoming:
        raise SystemExit("Wyoming must not request audible wake feedback")
    if "le_voice_listening_led_set(1)" not in wyoming:
        raise SystemExit("Wyoming capture must retain visual-only feedback")
    if "le_voice_listening_feedback_set(1)" not in local:
        raise SystemExit("local capture must retain audible wake feedback")
    if "le_voice_listening_feedback_set(0)" not in local:
        raise SystemExit("local capture must clear audible wake feedback")

    print("voice listening callers: Wyoming silent, local chirped: ok")


if __name__ == "__main__":
    main()
