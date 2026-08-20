#!/usr/bin/env python3
import pathlib

source = pathlib.Path("web/js/app.js").read_text(encoding="utf-8")

assert "const approved=['LibreEcho','Computer','Echo','Custom model']" in source
assert "current+' (current)'" in source
assert "Detection enablement is reported by the live wake-word adapter" in source
assert "toggle('Enable wake-word detection'" not in source
assert "wake_word:$('#wake-word').value.replace(/ \\(current\\)$/,'')" in source
print("wake-word live-state preservation: ok")
