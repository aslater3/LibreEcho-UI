#!/usr/bin/env python3
"""Browser behaviour worth running rather than grepping.

The Simulation page's radio barge-in pair, and the System update card's
loaded-image-size row.

Two layers. The source assertions below always run and pin the properties that
make the pair a pair rather than two unrelated phrases. The behavioural harness
next to this file drives the real page code against a fake device and needs
node; CI installs it, so a missing node here is a local gap, and it is reported
loudly rather than passing quietly.
"""
import pathlib
import shutil
import subprocess
import sys

source = pathlib.Path("web/js/app.js").read_text(encoding="utf-8")

# Ordered and adjacent: the stop half only means anything after the start half.
start = source.index("['Play the radio','Alexa, play the radio',simRadioStart]")
stop = source.index("['Stop the radio while it plays','Alexa, stop',simRadioStop]")
assert start < stop, "the start half must come first"
assert source[start:stop].count("\n") == 1, "the two halves must be adjacent"
assert source[stop:].lstrip().startswith(
    "['Stop the radio while it plays','Alexa, stop',simRadioStop]];"
), "the pair must be last, so nothing after it is measured over music"

# The verdict comes from the device, not from a log line.
assert "api('/integrations/radio')" in source
assert "api('/audio')" in source
assert "w.playing===false" in source and "w.amplifier_on===false" in source
assert "entry.voice_stopped=stoppedAt!==null" in source

# Cleanup is unconditional and in a finally, so a failed stop leaves it quiet.
assert "async function simRadioCleanup(){\n await api('/integrations/radio/stop'" in source
stop_fn = source[source.index("async function simRadioStop("):]
finally_block = stop_fn[stop_fn.index(" }finally{"):]
assert "await simRadioCleanup();" in finally_block[:400], "cleanup must run in the finally"

# The run-all sweep dispatches to a preset's own runner.
assert "for(const [label,phrase,runner] of SIM_PHRASES)" in source
assert "const run=runner||simRun;" in source

# The System update card shows the loaded image size against the daemon's cap.
assert 'id="update-size"' in source
assert "max_upload_bytes" in source and "'32 MiB'" not in source

# The music visualizer is on by default, and the read-back must not invert it.
# json_get_bool returns 0 for an absent key, so a `< 0` guard leaves the memset
# zero in place and reports "off" while the ring is reacting.
linux_backend = pathlib.Path("src/backend_linux.c").read_text(encoding="utf-8")
guard = linux_backend[linux_backend.index('json_get_bool(response, "visualizer_enabled"'):]
assert "&o->visualizer_enabled) <= 0)" in guard[:120], (
    "visualizer_enabled must default on via <= 0, not < 0")
assert "o->visualizer_enabled = 1;" in guard[:200]

print("web ui source contract: ok")

harness = "tests/web_ui_behaviour_harness.js"
if shutil.which("node") is None:
    print("web ui behaviour harness: SKIPPED, node is not installed", file=sys.stderr)
else:
    subprocess.run(["node", harness], check=True)
