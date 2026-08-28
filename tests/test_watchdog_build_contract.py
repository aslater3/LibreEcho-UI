#!/usr/bin/env python3
"""The watchdog objects must be removed by the repository clean target."""
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parent.parent
MAKEFILE = ROOT / "Makefile"


def main():
    text = MAKEFILE.read_text()
    clean_start = text.index("\nclean:")
    clean = text[clean_start:]
    if "$(WATCHDOGD_OBJECTS)" not in clean:
        print("clean target does not remove WATCHDOGD_OBJECTS", file=sys.stderr)
        return 1
    print("watchdog build clean contract: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())