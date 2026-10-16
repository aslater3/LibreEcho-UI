#!/bin/sh
# Standalone Ogg Opus decode tests for libreecho-radiod.
#
# The pinned Opus decode stack (libogg + libopus + libopusfile) is built
# outside this repository.  Point this harness at the resulting prefix:
#
#   OPUS_PREFIX=/path/to/prefix sh tests/run_radiod_opus_tests.sh
#   sh tests/run_radiod_opus_tests.sh --prefix /path/to/prefix
#
# The prefix must contain lib/lib{ogg,opus,opusfile}.a and the headers under
# include/ (opusfile.h, opus/*.h, ogg/*.h).  The enabled decode test drives
# the real decoder against generated fixtures; the disabled run proves the
# stub build refuses Opus with LE_RADIO_OPUS_UNSUPPORTED instead of pretending.
# No network and no ffmpeg are needed once tests/radiod_opus_fixture.h exists.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PREFIX=${OPUS_PREFIX:-}

while [ $# -gt 0 ]; do
    case $1 in
        --prefix) shift; [ $# -gt 0 ] || { echo "run_radiod_opus_tests.sh: --prefix needs a directory" >&2; exit 2; }; PREFIX=$1 ;;
        -h|--help) echo "usage: OPUS_PREFIX=DIR sh tests/run_radiod_opus_tests.sh [--prefix DIR]" >&2; exit 0 ;;
        *) echo "run_radiod_opus_tests.sh: unexpected argument: $1" >&2; exit 2 ;;
    esac
    shift
done

[ -n "$PREFIX" ] || {
    echo "run_radiod_opus_tests.sh: set OPUS_PREFIX to the pinned Opus prefix" >&2
    exit 2
}
[ -d "$PREFIX" ] || { echo "run_radiod_opus_tests.sh: not a directory: $PREFIX" >&2; exit 1; }

# Fail closed on an incomplete prefix rather than compiling against stubs.
missing=
for h in include/opus/opusfile.h include/opusfile.h; do
    [ -f "$PREFIX/$h" ] && have_opusfile_h=1
done
[ "${have_opusfile_h:-}" = 1 ] || missing="$missing include/opus/opusfile.h"
for h in include/opus/opus.h include/opus.h; do
    [ -f "$PREFIX/$h" ] && have_opus_h=1
done
[ "${have_opus_h:-}" = 1 ] || missing="$missing include/opus/opus.h"
for h in include/opus/opus_multistream.h include/opus_multistream.h; do
    [ -f "$PREFIX/$h" ] && have_ms_h=1
done
[ "${have_ms_h:-}" = 1 ] || missing="$missing include/opus/opus_multistream.h"
[ -f "$PREFIX/include/ogg/ogg.h" ] || missing="$missing include/ogg/ogg.h"
for a in lib/libopusfile.a lib/libopus.a lib/libogg.a; do
    [ -f "$PREFIX/$a" ] || missing="$missing $a"
done
[ -z "$missing" ] || { echo "run_radiod_opus_tests.sh: incomplete OPUS_PREFIX ($PREFIX):$missing" >&2; exit 1; }

FIXTURE="$ROOT/tests/radiod_opus_fixture.h"
if [ ! -f "$FIXTURE" ]; then
    if command -v python3 >/dev/null 2>&1 && command -v ffmpeg >/dev/null 2>&1; then
        python3 "$ROOT/tests/gen_radiod_opus_fixture.py" "$FIXTURE"
    else
        echo "run_radiod_opus_tests.sh: $FIXTURE is missing and cannot be regenerated (need python3 + ffmpeg)" >&2
        exit 1
    fi
fi

WORK=$(mktemp -d "${TMPDIR:-/tmp}/le-opus-tests.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

CC=${CC:-cc}
# libopusfile.h includes <opus_multistream.h>, so both include roots are needed.
CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Werror"
INCS="-I$PREFIX/include -I$PREFIX/include/opus -Isrc -Isrc/adapter -Itests"
LIBS="$PREFIX/lib/libopusfile.a $PREFIX/lib/libopus.a $PREFIX/lib/libogg.a -lm"

cd "$ROOT"

echo "opus tests: building radio_opus.c (disabled)"
"$CC" $CFLAGS $INCS -c src/adapter/radio_opus.c -o "$WORK/radio_opus_disabled.o"

echo "opus tests: building radio_opus.c (enabled, $PREFIX)"
"$CC" $CFLAGS -DLE_RADIOD_ENABLE_OPUS $INCS -c src/adapter/radio_opus.c -o "$WORK/radio_opus_enabled.o"

echo "opus tests: running disabled-capability test"
"$CC" $CFLAGS $INCS tests/test_radio_opus.c src/adapter/radio_opus.c -o "$WORK/opus_disabled"
"$WORK/opus_disabled"

echo "opus tests: running enabled decode test"
"$CC" $CFLAGS -DLE_RADIOD_ENABLE_OPUS $INCS tests/test_radio_opus.c \
    src/adapter/radio_opus.c $LIBS -o "$WORK/opus_enabled"
"$WORK/opus_enabled"

echo "opus tests: ok"
