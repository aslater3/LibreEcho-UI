#!/bin/sh
# Standalone focused harness for the canonical voice history core (#102).
#
# Compiles and runs the two units owned by the history change without touching
# the shared Makefile or tests/run_tests.sh:
#   - tests/test_voice_history.c          : the bounded RAM ring + serialization
#   - tests/test_voice_pipeline_outcomes.c: the pipeline pre-transcript callback
#
# Usage: tests/voice_history_harness.sh [output-dir]
set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:-"$repo/build/voice-history-harness"}
cc=${CC:-cc}
cflags="-D_POSIX_C_SOURCE=200809L -std=c99 -O2 -Wall -Wextra -Wpedantic -Werror -Isrc -Isrc/adapter"

mkdir -p "$out"
cd "$repo"

echo "== building test-voice-history =="
# shellcheck disable=SC2086
$cc $cflags tests/test_voice_history.c src/adapter/voice_history.c \
    -o "$out/test-voice-history"

echo "== building test-voice-pipeline-outcomes =="
# shellcheck disable=SC2086
$cc $cflags tests/test_voice_pipeline_outcomes.c \
    src/adapter/voice_pipeline.c src/adapter/voice_stream.c \
    src/adapter/voice_listening_led.c src/adapter/adapter_client.c \
    src/adapter/adapter_server.c src/json.c src/log.c -lpthread \
    -o "$out/test-voice-pipeline-outcomes"

echo "== running test-voice-history =="
"$out/test-voice-history"

echo "== running test-voice-pipeline-outcomes =="
"$out/test-voice-pipeline-outcomes"

echo "voice history core: all focused harnesses passed"
