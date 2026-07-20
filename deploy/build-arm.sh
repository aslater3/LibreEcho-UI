#!/bin/sh
# Build and validate the LibreEcho ARM32 release artifacts.

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$SCRIPT_DIR")
MAKE=${MAKE:-make}
FILE=${FILE:-file}
DEFAULT_CROSS_COMPILE=arm-linux-musleabihf-

if [ -n "${CC:-}" ]; then
    COMPILER=$CC
    case "$COMPILER" in
        gcc|cc)
            BUILD_CROSS_COMPILE=${CROSS_COMPILE:-$DEFAULT_CROSS_COMPILE}
            COMPILER_CHECK=${BUILD_CROSS_COMPILE}${COMPILER}
            ;;
        *)
            BUILD_CROSS_COMPILE=
            COMPILER_CHECK=$COMPILER
            ;;
    esac
else
    COMPILER=gcc
    BUILD_CROSS_COMPILE=${CROSS_COMPILE:-$DEFAULT_CROSS_COMPILE}
    COMPILER_CHECK=${BUILD_CROSS_COMPILE}gcc
fi

if ! command -v "$MAKE" >/dev/null 2>&1; then
    echo "error: make not found: $MAKE" >&2
    exit 1
fi
if ! command -v "$FILE" >/dev/null 2>&1; then
    echo "error: file command not found: $FILE" >&2
    exit 1
fi
if ! command -v "$COMPILER_CHECK" >/dev/null 2>&1; then
    echo "error: ARM compiler not found: $COMPILER_CHECK" >&2
    echo "       install arm-linux-musleabihf-gcc or set CC to a usable compiler" >&2
    exit 1
fi

cd "$ROOT"
"$MAKE" clean
"$MAKE" CROSS_COMPILE="$BUILD_CROSS_COMPILE" CC="$COMPILER" release

for binary in \
    build/libreecho-web \
    build/libreecho-networkd \
    build/libreecho-audiod \
    build/libreecho-ledd
do
    if [ ! -f "$binary" ]; then
        echo "error: expected binary was not produced: $binary" >&2
        exit 1
    fi

    description=$($FILE -b "$binary")
    case "$description" in
        *"ELF 32-bit"*"ARM"*)
            ;;
        *)
            echo "error: $binary is not an ARM 32-bit ELF: $description" >&2
            exit 1
            ;;
    esac

    size=$(wc -c <"$binary" | tr -d '[:space:]')
    echo "$binary: $size bytes ($description)"
done

echo "ARM32 release build verified successfully."
