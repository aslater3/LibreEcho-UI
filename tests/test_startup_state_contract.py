#!/usr/bin/env python3
"""Run startup behaviour against the scripts actually loaded by index.html."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
subprocess.run(["node", "tests/test_startup_state.js"], cwd=ROOT, check=True, timeout=10)
