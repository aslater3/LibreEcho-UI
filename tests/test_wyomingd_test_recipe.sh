#!/bin/sh
# Guard the private Wyoming watchdog fixture used by test-wyomingd:
#   1. the fixture daemon must compile with the repository C99 standard and
#      warning set (the same $(CSTD) $(WARN) the shipping objects use), so the
#      behavioral test exercises the real build configuration; and
#   2. `make clean` must remove the fixture artifact, otherwise the aggregate
#      flow reuses a stale binary after only a Makefile flag/recipe change.
#
# Both contracts are checked against the live Makefile recipes with a dry run
# (`make -Bn`), so this is an executable recipe contract rather than a source
# grep and needs no compilation.
set -eu

recipe=$(make -Bn build/libreecho-wyomingd-test 2>/dev/null | tr '\n' ' ')

case "$recipe" in
    *-std=c99*) : ;;
    *) echo "libreecho-wyomingd-test must compile with -std=c99 (CSTD)" >&2; exit 1 ;;
esac
case "$recipe" in
    *-Wall*) : ;;
    *) echo "libreecho-wyomingd-test must compile with warnings enabled (WARN)" >&2; exit 1 ;;
esac

clean=$(make -Bn clean 2>/dev/null | tr '\n' ' ')
case "$clean" in
    *libreecho-wyomingd-test*) : ;;
    *) echo "make clean must remove build/libreecho-wyomingd-test" >&2; exit 1 ;;
esac

printf '%s\n' 'wyomingd watchdog fixture recipe: C99 flags and clean removal: ok'
