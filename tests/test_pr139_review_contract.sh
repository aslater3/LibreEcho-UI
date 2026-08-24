#!/bin/sh
set -eu

grep -q 'clear-wifi-mac' web/js/app.js
grep -q "wifi_mac:''" web/js/app.js
grep -q 'assistant/history' src/api.c docs/API.md web/openapi.json
grep -q 'method_not_allowed(r);return;' src/api.c
grep -q 'if(simDeviceTurns.length)' web/js/app.js
grep -q 'wall_clock_milliseconds' src/adapter/agentd.c
grep -q 'CLOCK_REALTIME' src/adapter/agentd.c
grep -q 'history too large' src/adapter/agentd.c
grep -q 'body_limit = LE_ADAPTER_MSG_MAX - 256' src/adapter/agentd.c
! grep -q 'turn_history_next.*LE_AGENT_TURN_HISTORY - 1' src/adapter/agentd.c
grep -q 'LE_TLS_AVAILABLE' Makefile src/api.c
grep -q 'strlen(users_path)+strlen(".sessions")' src/api.c
grep -q 'BT_STATUS_BOND_NAME_MAX' src/adapter/btd.c
grep -q "activeElement.*mac-bt" web/js/bluetooth.js
grep -q 'SIM_DEVICE_CLEAR_KEY' web/js/app.js
grep -q 'address_configured' docs/API.md web/openapi.json
printf '%s\n' 'PR 139 review contracts: ok'
