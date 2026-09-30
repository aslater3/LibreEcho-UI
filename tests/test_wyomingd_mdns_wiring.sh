#!/bin/sh
# Historical runner entry: the old Wyoming discovery owner is gone. Exercise
# the new bounded client and actual native listener-before-lease ordering,
# including failed bind, EOF withdrawal and shared-supervisor restart.
set -eu
python3 tests/test_mdns_client.py
python3 tests/test_mdns_wyoming.py
printf '%s\n' 'ESPHome shared mDNS client/listener wiring: ok'
