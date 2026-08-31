#!/usr/bin/env python3
"""Regression contract for the sidebar repository link."""

from pathlib import Path

index = Path("web/index.html").read_text(encoding="utf-8")

expected_url = "https://github.com/aslater3/LibreEcho"
assert f'href="{expected_url}"' in index, "sidebar GitHub link points to the wrong repository"
assert f"<small>github.com/aslater3/LibreEcho</small>" in index, (
    "sidebar GitHub link displays the wrong repository"
)
assert 'href="https://github.com/libreecho"' not in index, (
    "stale unowned GitHub URL remains in the sidebar"
)

print("sidebar GitHub link contract: ok")
