#!/usr/bin/env python3
"""Regression contract for preserving Wi-Fi security through the Linux backend."""

from pathlib import Path
import re


SOURCE = Path(__file__).parents[1] / "src/backend_linux.c"


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    match = re.search(
        r"static int connect_wifi\(.*?\n}\n\nstatic int disconnect_wifi",
        source,
        re.DOTALL,
    )
    if not match:
        raise SystemExit("connect_wifi implementation was not found")
    function = match.group(0)
    required = (
        "char security[",
        "credentials->security",
        "json_escape(security",
        '\\"security\\":\\"%s\\"',
    )
    missing = [fragment for fragment in required if fragment not in function]
    if missing:
        raise SystemExit(
            "Linux Wi-Fi backend drops security selection: " + ", ".join(missing)
        )
    print("Linux Wi-Fi security propagation contract: PASS")


if __name__ == "__main__":
    main()
