#!/bin/sh
# Emit reproducible source provenance for builds and deployment manifests.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(dirname "$SCRIPT_DIR")
FIELD=all
JSON=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --field) [ "$#" -ge 2 ] || { echo "missing --field value" >&2; exit 2; }; FIELD=$2; shift ;;
        --json) JSON=1 ;;
        -h|--help) echo "Usage: $0 [--field commit|dirty|digest|all] [--json]"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

cd "$ROOT"
if ! git rev-parse --show-toplevel >/dev/null 2>&1; then
    commit=unknown
dirty=unknown
digest=unknown
else
    commit=$(git rev-parse HEAD 2>/dev/null || printf '%s' unknown)
    status=$(git status --porcelain --untracked-files=all)
    if [ -n "$status" ]; then dirty=1; else dirty=0; fi
    # Hash every tracked and non-ignored untracked file by content and path.
    digest=$(git ls-files -co --exclude-standard -z | sort -z | xargs -0 -r -n1 sh -c 'test -f "$1" && sha256sum "$1"' sh | sha256sum | cut -d' ' -f1)
fi

case "$FIELD" in
    commit) printf '%s\n' "$commit" ;;
    dirty) printf '%s\n' "$dirty" ;;
    digest) printf '%s\n' "$digest" ;;
    all)
        if [ "$JSON" -eq 1 ]; then
            printf '{"schema":1,"source_commit":"%s","source_dirty":%s,"source_digest":"%s"}\n' \
                "$commit" "$( [ "$dirty" = 1 ] && printf true || printf false )" "$digest"
        else
            printf 'SOURCE_COMMIT=%s\nSOURCE_DIRTY=%s\nSOURCE_DIGEST=%s\n' "$commit" "$dirty" "$digest"
        fi
        ;;
    *) echo "invalid field: $FIELD" >&2; exit 2 ;;
esac
