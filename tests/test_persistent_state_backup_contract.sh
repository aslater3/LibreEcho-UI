#!/bin/sh
# Backup coverage contract for the active persistent state.
#
# The backup tool scopes itself to a fixed pair of trees and prunes a fixed set
# of names. This test checks that scope against the code that actually owns the
# state, so a new persistent store, or a one-shot file a daemon consumes at
# boot, cannot be silently captured, restored, or dropped:
#
#   1. the captured trees are the consumer-owned config and secrets trees, and
#      the factory seed and log directory are never captured;
#   2. the reset path clears exactly those two trees;
#   3. every persistent path the sources and init scripts use is either inside a
#      captured tree or named in the manifest as excluded;
#   4. every persistent file an init script consumes one-shot is pruned, so a
#      restore cannot replay an already-consumed request;
#   5. symlinked state is refused on both create and restore, including a
#      configured root that is itself a link or is spelled with a `.`/`..`
#      component;
#   6. the exclusion contract is applied by restore as well as by create, so an
#      archive written by an earlier tool, or one whose manifest names no
#      exclusions, cannot install a pruned file;
#   7. the top-level manifest must be a regular file that is not a link, and a
#      member that escapes the archive root is refused before extraction.
#
# This is a source-contract test: it proves the tool describes the same state as
# its consumers, not that a backup ran on a device.
set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TOOL=$REPO/tools/libreecho-backup.sh
RESET_CONTRACT=$REPO/tests/test_factory_reset.c
DATA_ROOT=/data/libreecho
CONFIG_DIR=$DATA_ROOT/config
SECRETS_DIR=$DATA_ROOT/secrets

fail() {
    printf 'persistent state backup contract: %s\n' "$*" >&2
    exit 1
}

require_in_tool() {
    grep -Fq -- "$1" "$TOOL" || fail "the backup tool does not declare: $1"
}

[ -r "$TOOL" ] || fail "backup tool is missing: $TOOL"

# 1. Captured trees.
require_in_tool "DATA_ROOT=\${LIBREECHO_DATA_ROOT:-$DATA_ROOT}"
require_in_tool "CONFIG_DIR=\${LIBREECHO_CONFIG_DIR:-\${CONFIG_DIR:-\$DATA_ROOT/config}}"
require_in_tool "SECRETS_DIR=\${LIBREECHO_SECRETS_DIR:-\${SECRETS_DIR:-\$DATA_ROOT/secrets}}"
require_in_tool 'factory-seed:/etc/libreecho'
case "$CONFIG_DIR$SECRETS_DIR" in
    /etc/*|/var/*) fail 'a captured tree is not consumer-owned' ;;
esac

# 3. Every persistent path in the sources and init scripts is accounted for.
paths=$(grep -rhoE '/data/libreecho/[A-Za-z0-9._/-]+' "$REPO/src" "$REPO/init" |
    sed 's:/*$::' | sort -u)
[ -n "$paths" ] || fail 'no persistent paths were found; the scan is broken'
outside=0
for path in $paths; do
    relative=${path#/data/libreecho/}
    case "$relative" in
        ''|config|secrets|config/*|secrets/*) continue ;;
    esac
    first=${relative%%/*}
    grep -Fq "/data/libreecho/$first" "$TOOL" ||
        fail "persistent path is neither captured nor excluded: $path"
    outside=$((outside + 1))
done
[ "$outside" -gt 0 ] ||
    fail 'no out-of-scope persistent path was checked; the scan is too narrow'

# 2. Reset clears exactly the captured trees.
reset_children=$(grep -o 'clear_directory(root_fd, "[A-Za-z0-9._-]*")' \
    "$REPO/src/factory_reset.c" | sed 's/.*"\(.*\)".*/\1/' | sort | tr '\n' ' ')
[ "$reset_children" = 'config secrets ' ] ||
    fail "factory reset clears unexpected directories: $reset_children"

reset_list=$(sed -n '/reset_files\[\] = {/,/};/p' "$RESET_CONTRACT")
[ -n "$reset_list" ] || fail 'the reset contract has no reset_files list'
reset_files=$(printf '%s\n' "$reset_list" |
    grep -oE '"(config|secrets)/[A-Za-z0-9._-]+"' | tr -d '"' | sort -u)
[ -n "$reset_files" ] || fail 'the reset contract lists no persistent files'
for reset_file in $reset_files; do
    case "$reset_file" in
        config/*|secrets/*) ;;
        *) fail "reset clears state outside the captured trees: $reset_file" ;;
    esac
done

# 4. Files an init script consumes one-shot must be pruned from any archive.
pruned=0
for script in "$REPO"/init/*.init; do
    for variable in $(grep -o 'rm -f "\$[A-Za-z_]*"' "$script" |
        sed 's/.*\$\([A-Za-z_]*\)".*/\1/' | sort -u); do
        default=$(sed -n "s/^$variable=\${$variable:-\([^}]*\)}.*/\1/p" "$script")
        case "$default" in
            "$DATA_ROOT"/*) ;;
            *) continue ;;
        esac
        grep -Fq -- "\$root/${default##*/}" "$TOOL" ||
            fail "one-shot persistent file is not pruned by its exact path: $default"
        if grep -Fq -- "-name '${default##*/}'" "$TOOL"; then
            fail "one-shot persistent file is pruned by basename anywhere: $default"
        fi
        pruned=$((pruned + 1))
    done
done
[ "$pruned" -gt 0 ] ||
    fail 'no one-shot persistent file was checked; the scan is too narrow'

# 5. Symlinked state is refused in both directions.
require_in_tool 'refuse_symlinks "$tmpdir/persistent/config"'
require_in_tool 'refuse_symlinks "$tmpdir/persistent/secrets"'
require_in_tool 'refuse_symlinks "$tmpdir/persistent"'
require_in_tool 'refuse_linked_roots'
require_in_tool 'has_dot_component "$root"'
require_in_tool 'has_linked_ancestor "$root"'
require_in_tool 'persistent state root is a symbolic link'
require_in_tool 'persistent state root uses a dot component'
require_in_tool 'persistent state root crosses a symbolic link'
require_in_tool 'transaction-files:config,secrets:*.tmp,*.new,*.bak'
require_in_tool 'require_plain_manifest "$root/manifest.json"'
require_in_tool 'invalid backup (manifest is a symbolic link)'
require_in_tool 'invalid backup (manifest is not a regular file)'
require_in_tool 'refuse_archive_escape "$backup"'
require_in_tool 'backup member escapes the archive root'
require_in_tool 'Excluded by contract, never restored'

# 6. Create and restore apply one shared prune policy, to the trees create
# stages and to the trees every restore unpacks.
for call in \
    'prune_transaction_files "$tmpdir/persistent/config"' \
    'prune_transaction_files "$tmpdir/persistent/secrets"' \
    'prune_one_shot_config_files "$tmpdir/persistent/config"' \
    'prune_transaction_files "$trees/config"' \
    'prune_transaction_files "$trees/secrets"' \
    'prune_one_shot_config_files "$trees/config"' \
    'apply_restore_exclusions "$tmpdir/persistent"'; do
    require_in_tool "$call"
done
classes=$(grep -c "name '\*.bak'" "$TOOL")
[ "$classes" -eq 1 ] ||
    fail "the stale-copy prune class is defined $classes times; the two paths can drift"
# The class exists because these owners leave the previous contents of a
# committed file beside it inside the captured trees; pruning by name covers
# all of them, and this pins the reason the class is not a fixed path list.
for writer in src/config_store.c src/adapter/agentd.c src/adapter/timerd.c; do
    grep -Fq '.bak' "$REPO/$writer" ||
        fail "the stale-copy class no longer covers a writer: $writer"
done

# The exclusions are applied before the prompt, so nothing is replaced with a
# file the contract never restores, and the prompt describes what is installed.
exclusion_line=$(grep -n 'apply_restore_exclusions "\$tmpdir/persistent"' "$TOOL" |
    head -n1 | cut -d: -f1)
prompt_line=$(grep -n 'Restore active persistent state from' "$TOOL" |
    head -n1 | cut -d: -f1)
stage_line=$(grep -n 'stage_restore_trees "\$tmpdir/persistent/config"' "$TOOL" |
    head -n1 | cut -d: -f1)
[ -n "$exclusion_line" ] && [ -n "$prompt_line" ] && [ -n "$stage_line" ] ||
    fail 'the restore sequence is not declared where the contract expects it'
[ "$exclusion_line" -lt "$prompt_line" ] && [ "$prompt_line" -lt "$stage_line" ] ||
    fail 'restore does not apply the exclusions before it changes live state'

printf '%s\n' 'persistent state backup contract: ok'
