#!/bin/sh
set -eu

# Review contract for the bounded, observable voice-pipeline restart.
grep -q 'voice_pipeline_restart_pid' src/api.c
grep -q 'waitpid(voice_pipeline_restart_pid, &status, WNOHANG)' src/api.c
grep -q 'voice_pipeline_restart_pending' src/api.c
grep -q 'r->status = 202' src/api.c
grep -q 'A voice pipeline restart is already in progress' src/api.c
grep -q 'restart.state' docs/API.md web/openapi.json
grep -q '"202"' web/openapi.json
grep -q '"409"' web/openapi.json
# The restart must remain a single tracked child, not an untracked double-fork.
! grep -q 'pid_t worker = fork();' src/api.c
! grep -q 'setsid()' src/api.c
printf '%s\n' 'voice pipeline restart bounds and status contract: ok'
