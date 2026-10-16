#!/usr/bin/env python3
"""Execute the real wake page and save handler without a browser dependency."""
import subprocess

subprocess.run(["node", "tests/test_wake_word_ui.js"], check=True)
