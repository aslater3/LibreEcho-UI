#!/usr/bin/env python3
"""Regression contracts for the browser Baby Monitor stream lifecycle."""

from pathlib import Path


SOURCE = Path(__file__).parents[1] / "web/js/app.js"


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    required = (
        "response.headers.get('X-LibreEcho-Audio')",
        "pcm_s16_le",
        "babyStream.generation",
        "const generation=babyStream.generation",
        "if(generation!==babyStream.generation)",
    )
    missing = [fragment for fragment in required if fragment not in text]
    if missing:
        raise SystemExit(
            "missing Baby Monitor stream contract: " + ", ".join(missing)
        )
    print("baby monitor stream format and lifecycle contract: PASS")


if __name__ == "__main__":
    main()
