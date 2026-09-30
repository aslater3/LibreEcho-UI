#!/usr/bin/env python3
"""Regression contracts for the browser Baby Monitor stream lifecycle."""

from pathlib import Path


ROOT = Path(__file__).parents[1]
SOURCE = ROOT / "web/js/app.js"
SERVER = ROOT / "src/http_server.c"


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
    server = SERVER.read_text(encoding="utf-8")
    for header in (
        "Content-Type: audio/L16; rate=16000; channels=1",
        "Content-Type: audio/L24; rate=16000; channels=9",
        "Transfer-Encoding: chunked",
        "Accept-Ranges: none",
        "X-LibreEcho-Audio: pcm_s16_le;rate=16000;channels=1",
        "X-LibreEcho-Audio: pcm_s24_3le;rate=16000;channels=9",
        "media-src 'self' data: blob:",
    ):
        if header not in server:
            raise SystemExit("missing Baby Monitor HTTP stream header: " + header)
    if "0\\r\\n\\r\\n" not in server:
        raise SystemExit("missing bounded chunked-stream terminator")
    for marker in ("const BABY_SILENCE_WAV=", "function babyMediaUnlock()", "babyStream.media=babyMediaUnlock()"):
        if marker not in text:
            raise SystemExit("missing iOS media-session unlock: " + marker)
    print("baby monitor stream format and lifecycle contract: PASS")


if __name__ == "__main__":
    main()
