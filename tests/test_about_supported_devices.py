#!/usr/bin/env python3
"""Regression contract for the supported-devices table on the About page.

Someone deciding whether to flash their device reads this table and acts on it,
so the distinction between confirmed and untested has to survive edits. The
risk is not that the table disappears -- that is obvious -- but that a model
quietly moves into "Confirmed" without anyone having booted LibreEcho on it.
"""

from pathlib import Path

app = Path("web/js/app.js").read_text(encoding="utf-8")

start = app.index("function aboutPage(")
about = app[start:app.index("\n", start)]

assert "Supported devices" in about, "About page no longer lists supported devices"

# Confirmed means someone booted it. Both of these have been run on real hardware.
for device, codename in (
    ("Echo (2nd generation)", "radar_puffin"),
    ("Echo Dot (2nd generation)", "biscuit_puffin"),
):
    assert device in about, f"{device} missing from the supported-devices table"
    assert codename in about, f"{codename} missing from the supported-devices table"

# Everything else is untested, and must say so rather than implying support.
for codename in ("knight_puffin", "crown_puffin", "pumpkin_puffin", "gemini_puffin"):
    assert codename in about, f"{codename} missing from the supported-devices table"

assert about.count("'Confirmed'") == 2, (
    "exactly two devices are confirmed working; a model has been promoted to "
    "Confirmed without hardware evidence, or a confirmed one was dropped"
)
assert "Likely, untested" in about, (
    "untested devices must be labelled untested rather than shown as supported"
)
assert "Nothing outside this list is supported" in about, (
    "the About page no longer warns that other devices have no install path"
)

print("about supported-devices contract: ok")
