#!/bin/sh
set -eu

grep -q 'Connect LibreEcho to <span class="no-wrap">Wi-Fi</span>\.' web/setup.html
grep -q '\.no-wrap{white-space:nowrap}' web/css/setup.css
grep -q 'id="vendor-firmware-state"' web/setup.html
grep -q 'id="force-vendor-import"' web/setup.html
grep -q 'if(!le_get_network_state(c->backend,&next_network))n=next_network' src/api.c
! grep -q 'Setup state is unavailable' src/api.c
grep -q "'/setup/vendor-import-force-next-boot'" web/js/setup.js
grep -q 'force-unverified-owner-local-import' web/js/setup.js
grep -q 'vendor_firmware' src/api.c
grep -q 'snprintf(w.wake_word,sizeof(w.wake_word),"LibreEcho")' src/api.c
grep -q 'if(!le_get_wake_word_state(c->backend,&next_wake))w=next_wake' src/api.c
grep -q 'setup.hardwareReady=!!current.wlan0_registered' web/js/setup.js
grep -q 'wlan0_registered' src/api.c
grep -q '/api/v1/setup/vendor-import-force-next-boot' src/api.c
grep -q 'GET /api/v1/setup' docs/API.md
grep -q 'POST /api/v1/setup/vendor-import-force-next-boot' docs/API.md
grep -q 'force-unverified-owner-local-import' docs/API.md
printf '%s\n' 'setup connectivity status and layout contract: ok'
