#!/bin/sh
set -eu

SOURCE=${1:-src/adapter/bt_profile.c}

awk '
    /static void sbc_decode_chunk\(/ { in_decoder = 1 }
    in_decoder {
        opens = gsub(/\{/, "{")
        closes = gsub(/\}/, "}")
        print
        depth += opens - closes
        if (depth == 0 && opens + closes > 0)
            exit
    }
' "$SOURCE" >./build/test-bluetooth-decoder-state.c

if grep -q 'sbc_reinit' ./build/test-bluetooth-decoder-state.c; then
    echo 'decoder state contract: per-frame sbc_reinit remains' >&2
    exit 1
fi
grep -q 'sbc_init(&session->sbc, 0)' ./build/test-bluetooth-decoder-state.c
grep -q 'sbc_decode(&session->sbc' ./build/test-bluetooth-decoder-state.c
echo 'bluetooth decoder state contract: ok'