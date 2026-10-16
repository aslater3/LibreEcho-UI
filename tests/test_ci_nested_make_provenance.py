#!/usr/bin/env python3
"""A nested make must reuse the parent source snapshot, not redefine its macros.

CI exports ``CPPFLAGS`` with the caller's include paths. The top-level
``make test`` appends the once-per-invocation ``LE_SOURCE_*`` provenance macros
and hands ``CPPFLAGS`` to ``tests/run_tests.sh``, which drives many nested
``make`` invocations. When the suite changes tracked sources, a nested make used
to recompute a *different* snapshot and append conflicting ``-DLE_SOURCE_*``
definitions. ``build/test-esphome-radio-controls`` compiles with ``-Werror``, so
the redefinition became a hard CI failure.

This regression copies the working tree into a private repository, dirties a
tracked source between a parent ``make`` and a recursively invoked child ``make``
on the strict radio-controls target, and requires the child to succeed while
keeping the caller's ``CPPFLAGS`` include and the provenance macros intact.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROBE = """\
# Private orchestration for the nested-make provenance regression. It is copied
# into a throwaway repository and never shipped.
parent:
	@printf '\\n/* nested-make provenance probe */\\n' >> src/adapter/radiod.c
	@$(MAKE) --no-print-directory -f Makefile -f ci-nested-provenance.mk child
child: build/test-esphome-radio-controls
"""


def _clean_env() -> dict:
    """Drop inherited build/git state so the private copy is self-contained."""
    return {key: value for key, value in os.environ.items()
            if not key.startswith(("GIT_", "SOURCE_", "MAKE"))}


def _copy_working_tree(dest: Path) -> None:
    """Copy exactly the provenance-hashed file set (tracked + non-ignored)."""
    listed = subprocess.check_output(
        ["git", "-C", str(ROOT), "ls-files", "-z", "-co", "--exclude-standard"])
    for raw in listed.split(b"\0"):
        if not raw:
            continue
        source = ROOT / raw.decode()
        if not source.is_file():
            continue
        target = dest / raw.decode()
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def _git(*args, cwd, env):
    return subprocess.run(["git", *args], cwd=cwd, env=env, check=True,
                          text=True, capture_output=True)


class NestedMakeProvenance(unittest.TestCase):
    def test_nested_make_reuses_parent_snapshot_without_redefining_macros(self):
        with tempfile.TemporaryDirectory(prefix="le-nested-provenance-") as work:
            copy = Path(work) / "tree"
            copy.mkdir()
            _copy_working_tree(copy)
            env = _clean_env()
            _git("init", "-q", cwd=copy, env=env)
            _git("add", "-A", cwd=copy, env=env)
            _git("-c", "user.email=ci@example.invalid",
                 "-c", "user.name=ci", "commit", "-qm", "snapshot",
                 cwd=copy, env=env)
            (copy / "ci-nested-provenance.mk").write_text(PROBE)
            include = copy / "fake-include"
            include.mkdir()
            env["CPPFLAGS"] = f"-I{include}"

            result = subprocess.run(
                ["make", "-f", "Makefile", "-f", "ci-nested-provenance.mk", "parent"],
                cwd=copy, env=env, text=True, capture_output=True, timeout=600)
            output = result.stdout + result.stderr

            self.assertNotIn("redefined", output)
            self.assertEqual(result.returncode, 0, output)
            # The caller's include path must survive into the nested compile.
            self.assertIn(f"-I{include}", output)
            # Provenance must still be compiled in, not dropped to dodge the clash.
            self.assertIn("-DLE_SOURCE_DIGEST=", output)


if __name__ == "__main__":
    unittest.main()
