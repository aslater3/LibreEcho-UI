#!/bin/sh
# 0.14 feature-batch host build helper.
#
# Stages an Ogg Opus decode prefix and runs the enabled radiod Opus decoder
# test, then proves that switching the decoder on and off rebuilds the relevant
# objects (no stale `opus:true`/`opus:false`).
#
#   sh tests/test_feature_batch_build.sh [--prefix DIR]
#
# Prefix resolution, in order:
#   1. --prefix DIR or $OPUS_PREFIX, when it is a complete pinned prefix built
#      by the Platform tool tools/mt8163-arm32/ui/build_opus.sh;
#   2. otherwise the host's static libogg/libopus/libopusfile development
#      packages are staged into build/opus-host-prefix.
#
# Nothing is downloaded and the ordinary build never fetches source. When no
# complete prefix can be assembled the helper reports it: in CI (CI set) that
# is a hard failure so the enabled decoder test is never silently skipped.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

PREFIX=${OPUS_PREFIX:-}
while [ $# -gt 0 ]; do
    case $1 in
        --prefix) shift; [ $# -gt 0 ] || { echo "test_feature_batch_build.sh: --prefix needs a directory" >&2; exit 2; }; PREFIX=$1 ;;
        -h|--help) echo "usage: OPUS_PREFIX=DIR sh tests/test_feature_batch_build.sh [--prefix DIR]" >&2; exit 0 ;;
        *) echo "test_feature_batch_build.sh: unexpected argument: $1" >&2; exit 2 ;;
    esac
    shift
done

complete_prefix() {
    p=$1
    [ -n "$p" ] && [ -d "$p" ] || return 1
    for a in lib/libopusfile.a lib/libopus.a lib/libogg.a; do
        [ -f "$p/$a" ] || return 1
    done
    for h in include/opus/opusfile.h include/opus/opus.h \
             include/opus/opus_multistream.h include/ogg/ogg.h; do
        [ -f "$p/$h" ] || return 1
    done
    return 0
}

# Assemble a prefix from the host static development packages. This does not
# re-implement build_opus.sh: it only copies already-installed archives and
# headers, and fails if the static archives are absent.
stage_system_prefix() {
    out="$ROOT/build/opus-host-prefix"
    lf=$(cc -print-file-name=libopusfile.a)
    la=$(cc -print-file-name=libopus.a)
    lg=$(cc -print-file-name=libogg.a)
    for f in "$lf" "$la" "$lg"; do
        case $f in /*) [ -f "$f" ] || return 1 ;; *) return 1 ;; esac
    done
    for h in /usr/include/opus/opusfile.h /usr/include/opus/opus.h \
             /usr/include/opus/opus_multistream.h /usr/include/ogg/ogg.h; do
        [ -f "$h" ] || return 1
    done
    rm -rf "$out"
    mkdir -p "$out/include/opus" "$out/include/ogg" "$out/lib"
    cp -a /usr/include/ogg/. "$out/include/ogg/"
    cp -a /usr/include/opus/. "$out/include/opus/"
    cp "$lf" "$out/lib/libopusfile.a"
    cp "$la" "$out/lib/libopus.a"
    cp "$lg" "$out/lib/libogg.a"
    PREFIX=$out
    return 0
}

if complete_prefix "${PREFIX:-/nonexistent}"; then
    echo "feature-batch build: using Opus prefix $PREFIX"
else
    PREFIX=
    if stage_system_prefix; then
        echo "feature-batch build: staged host Opus prefix from static dev packages: $PREFIX"
    elif [ -n "${CI:-}" ]; then
        echo "feature-batch build: no complete Opus prefix and no static host packages;" \
             "install libogg-dev libopus-dev libopusfile-dev" >&2
        exit 1
    else
        echo "feature-batch build: SKIPPED (no Opus prefix; set OPUS_PREFIX or" \
             "install libogg-dev libopus-dev libopusfile-dev)"
        exit 0
    fi
fi

echo "== enabled radiod Opus decode test =="
sh "$ROOT/tests/run_radiod_opus_tests.sh" --prefix "$PREFIX"

# The decoder state is a compile-time macro on radiod.c and radio_opus.c
# together. Build enabled, then disabled, and require the second build to have
# rebuilt both objects: the stamp is what stops a stale object from publishing
# the wrong `opus` capability.
echo "== enable/disable flag-stamp protection =="
make RADIOD_OPUS_PREFIX="$PREFIX" build/libreecho-radiod >/dev/null
nm build/libreecho-radiod | grep -q ' T op_open_file' || {
    echo "feature-batch build: enabled radiod is missing the Opus decoder" >&2; exit 1; }
if grep -aq 'without an Opus decoder' build/libreecho-radiod; then
    echo "feature-batch build: enabled radiod still carries the disabled-stub status path" >&2; exit 1
fi

make build/libreecho-radiod >/dev/null
if nm build/libreecho-radiod | grep -q ' T op_open_file'; then
    echo "feature-batch build: disabling RADIOD_OPUS_PREFIX left a stale enabled radiod (flag stamp failed)" >&2
    exit 1
fi
grep -aq 'without an Opus decoder' build/libreecho-radiod || {
    echo "feature-batch build: disabled radiod does not report the honest stub" >&2; exit 1; }

echo "feature-batch build: ok (enabled Opus decode + flag stamp)"
