#!/bin/sh
# ESPHome readiness: a live daemon plus actual status, never port-only inference.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
python3 tests/test_esphome_control.py protocol
python3 tests/test_esphome_control_init.py
