#!/usr/bin/env python3
"""PR 244 contract: the silent-wake feedback regressions must actually run in
the aggregate suite.

The dedicated `test-voice-listening-feedback` recipe already executes both the C
behavioral test and the Python caller contract. The aggregate runner used to
only *build* the C binary, so `make test` could pass even if the Home Assistant
path chirped again. Guard that both regressions are invoked from run_tests.sh.
"""
from pathlib import Path

runner = Path("tests/run_tests.sh").read_text(encoding="utf-8")
makefile = Path("Makefile").read_text(encoding="utf-8")

# The C behavioral regression is built and executed by the aggregate suite.
assert "make build/test-voice-listening-feedback" in runner
assert "./build/test-voice-listening-feedback" in runner

# The Python caller contract is executed by the aggregate suite too.
assert "python3 tests/test_voice_listening_callers.py" in runner

# The recipe keeps both invocations as the single source of truth.
assert "test-voice-listening-feedback:" in makefile
assert "./$(BUILD)/test-voice-listening-feedback" in makefile
assert "python3 tests/test_voice_listening_callers.py" in makefile

# `make test` starts with `make clean`, so the newly introduced binary must be
# removed there too; otherwise a reused workspace re-runs a stale executable
# after only a header/compiler-flag change.
clean_recipe = makefile.split("\nclean:\n", 1)[1].split("\n\n", 1)[0]
assert "$(BUILD)/test-voice-listening-feedback" in clean_recipe
# The clean recipe must still remove the sibling voice regressions it already owned.
assert "$(BUILD)/test-voice-playback" in clean_recipe

print("PR 244 aggregate runner feedback wiring: ok")
print("PR 244 clean-build hygiene for the feedback binary: ok")
