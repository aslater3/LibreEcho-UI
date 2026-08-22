#!/bin/bash
# Logging wrapper around the real curl, used as agentd's --curl helper in
# live-LLM mode.  agentd pipes the request body on stdin; we keep a copy so
# the harness can still assert what the model was asked, then hand the body
# to curl unchanged.
set -uo pipefail
LOG=${LE_HARNESS_LLM_LOG:-/dev/null}
BODY=$(cat)
printf '%s\n' "$BODY" >>"$LOG"
printf '%s' "$BODY" | exec /usr/bin/curl "$@"
