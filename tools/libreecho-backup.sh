#!/bin/sh
# LibreEcho active persistent-state backup/restore tool.
#
# The factory image under /etc/libreecho is a seed, not live state.  This tool
# deliberately scopes itself to the consumer-owned /data/libreecho config and
# secrets directories.  Feature payloads, OTA state, release identity, logs,
# runtime state, transaction files (*.tmp, *.new), stale pre-update copies
# (*.bak), one-shot markers, and symlinked state are outside this backup
# contract: they are never captured and never restored.  The same exclusion and
# prune policy runs against the incoming trees of every restore, so an archive
# written by an older tool, or one whose manifest names no exclusions at all,
# cannot reintroduce them.

set -eu
umask 077

BACKUP_VERSION=2
DATA_ROOT=${LIBREECHO_DATA_ROOT:-/data/libreecho}
CONFIG_DIR=${LIBREECHO_CONFIG_DIR:-${CONFIG_DIR:-$DATA_ROOT/config}}
SECRETS_DIR=${LIBREECHO_SECRETS_DIR:-${SECRETS_DIR:-$DATA_ROOT/secrets}}
SERVICE_DIR=${LIBREECHO_SERVICE_DIR:-/etc/init.d}
SKIP_SERVICES=${LIBREECHO_SKIP_SERVICES:-0}
CONFIG_OWNER=${LIBREECHO_CONFIG_OWNER:-}
SECRETS_OWNER=${LIBREECHO_SECRETS_OWNER:-}

# A trailing slash makes `test -L` resolve the final component through the
# link, so a root spelled `/mnt/state/`, `/mnt/state//` or `/mnt/state/.` would
# pass the symlink refusal below. The roots are normalized before any check or
# copy.
trim_slashes() {
    root=$1
    while [ -n "${root%/}" ] && [ "${root%/}" != "$root" ]; do
        root=${root%/}
    done
    printf '%s\n' "$root"
}

# `test -L` reports false for a spelling that ends in a dot component as well:
# `/mnt/state/.` names the directory the link points at, `..` walks back out
# through the link to its target's parent, and a relative `./` or `../` root
# resolves through the working directory. A root that uses one is refused
# rather than rewritten: the tool never canonicalizes a configured root, so a
# directory whose name merely starts with a dot (`.state`) or one that shares a
# name prefix with another root (`state-old`) is still an ordinary root.
has_dot_component() {
    remainder=$1
    while [ -n "$remainder" ]; do
        component=${remainder%%/*}
        case "$component" in
            .|..) return 0 ;;
        esac
        case "$remainder" in
            */*) remainder=${remainder#*/} ;;
            *) return 1 ;;
        esac
    done
    return 1
}

# `test -L` on the root itself is not enough: an ancestor component that is a
# link (with `LIBREECHO_DATA_ROOT=/mnt/link/state`, only `/mnt/link` is one)
# redirects the copy and the restore into the link target while the final
# component stays a plain directory. Every ancestor is inspected with `test -L`,
# which never follows the link, so no component can move the state tree; the
# final component keeps its own message below. Nothing is resolved here.
has_linked_ancestor() {
    root=$1
    remainder=$root
    prefix=
    case "$remainder" in
        /*) remainder=${remainder#/} ;;
        *) prefix=. ;;
    esac
    while [ -n "$remainder" ]; do
        component=${remainder%%/*}
        case "$remainder" in
            */*) remainder=${remainder#*/} ;;
            *) remainder= ;;
        esac
        [ -n "$component" ] || continue
        if [ -z "$prefix" ]; then
            prefix=/$component
        else
            prefix=$prefix/$component
        fi
        # The final component is the root itself, reported by the caller.
        [ -n "$remainder" ] || break
        [ ! -L "$prefix" ] || {
            printf 'Error: persistent state root crosses a symbolic link: %s\n' "$prefix" >&2
            return 1
        }
    done
    return 0
}

DATA_ROOT=$(trim_slashes "$DATA_ROOT")
CONFIG_DIR=$(trim_slashes "$CONFIG_DIR")
SECRETS_DIR=$(trim_slashes "$SECRETS_DIR")

# Stop the supervisor before its writers; resume in reverse dependency order.
# Missing image services are allowed. Installed but inactive services stay off.
SERVICE_NAMES="libreecho-watchdogd libreecho-web libreecho-agentd libreecho-waked libreecho-airplayd libreecho-ttsd libreecho-sttd libreecho-btd libreecho-radiod libreecho-timerd libreecho-buttond libreecho-ledd libreecho-micd libreecho-audiod libreecho-networkd libreecho-timed"
STOPPED_SERVICES=

usage() {
    printf '%s\n' \
        "Usage: $0 {create|restore|list} [path]" \
        "" \
        "  create [path]    Back up active persistent state" \
        "  restore <path>   Restore active persistent state" \
        "  list <path>      List backup contents" \
        "" \
        "The archive contains /data/libreecho/config and /data/libreecho/secrets." \
        "Transaction files (*.tmp, *.new, *.bak) and one-shot wake or vendor" \
        "markers are excluded from create and from every restore."
    exit 1
}

fail() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

cleanup_dir() {
    [ -n "${1:-}" ] && [ -d "$1" ] || return 0
    rm -rf "$1"
}

require_active_state() {
    [ -d "$DATA_ROOT" ] || fail "active data root is unavailable: $DATA_ROOT"
    [ -d "$CONFIG_DIR" ] || fail "active config directory is unavailable: $CONFIG_DIR"
    [ -d "$SECRETS_DIR" ] || fail "active secrets directory is unavailable: $SECRETS_DIR"
    [ -f "$CONFIG_DIR/web-config.json" ] || fail "required active config is unavailable: $CONFIG_DIR/web-config.json"
    [ -f "$CONFIG_DIR/users" ] || fail "required account state is unavailable: $CONFIG_DIR/users"
    [ -r "$CONFIG_DIR/web-config.json" ] || fail "required active config is unreadable: $CONFIG_DIR/web-config.json"
    [ -r "$CONFIG_DIR/users" ] || fail "required account state is unreadable: $CONFIG_DIR/users"
}

# Persistent state is plain files and directories. A symlink in the tree is
# either a broken backup or an attempt to make restored state point somewhere
# else, and the daemons read these paths as root, so create and restore both
# refuse it rather than reproduce it. `cp -R` and `tar` preserve links, and the
# ownership pass has no type filter, so a link would otherwise be copied,
# chowned through its target, and followed by the next reader.
refuse_symlinks() {
    root=$1
    [ -d "$root" ] || return 1
    # Bounded: -quit stops at the first match, so the scan never collects the
    # tree, and its status is kept: a scan that could not complete is not a
    # "no links" result.
    links=$(find "$root" -type l -print -quit) || return 1
    [ -z "$links" ] || {
        printf 'Error: symbolic link in persistent state: %s\n' "$links" >&2
        return 1
    }
    return 0
}

# A root that is itself a symlink is dereferenced by `cp -R` before the staged
# tree can be scanned, so the configured roots are refused at the source, and so
# is any spelling of one that uses a `.` or `..` component or a link in its own
# path.
refuse_linked_roots() {
    for root in "$DATA_ROOT" "$CONFIG_DIR" "$SECRETS_DIR"; do
        if has_dot_component "$root"; then
            printf 'Error: persistent state root uses a dot component: %s\n' "$root" >&2
            return 1
        fi
        has_linked_ancestor "$root" || return 1
        [ ! -L "$root" ] || {
            printf 'Error: persistent state root is a symbolic link: %s\n' "$root" >&2
            return 1
        }
    done
    return 0
}

copy_tree() {
    source=$1
    target=$2
    [ -d "$source" ] || return 1
    mkdir -p "$target" || return 1
    cp -R "$source/." "$target/" || return 1
}

secure_tree() {
    root=$1
    [ -d "$root" ] || return 1
    find "$root" -type d -exec chmod 700 {} + || return 1
    find "$root" -type f -exec chmod 600 {} + || return 1
}

numeric_owner() {
    owner=$1
    case "$owner" in
        *:*) uid=${owner%%:*}; gid=${owner#*:} ;;
        *) return 1 ;;
    esac
    case "$uid$gid" in
        ''|*[!0-9]*) return 1 ;;
    esac
    printf '%s:%s\n' "$uid" "$gid"
}

target_owner() {
    target=$1
    configured=$2
    existing=
    if [ -e "$target" ]; then
        [ -d "$target" ] || return 1
        existing=$(stat -c '%u:%g' "$target") || return 1
    fi
    if [ -n "$configured" ]; then
        owner=$(numeric_owner "$configured") || {
            printf 'Error: configured owner is not numeric uid:gid: %s\n' "$configured" >&2
            return 1
        }
        if [ -n "$existing" ] && [ "$owner" != "$existing" ]; then
            printf 'Error: configured owner %s does not match existing owner %s: %s\n' \
                "$owner" "$existing" "$target" >&2
            return 1
        fi
    else
        owner=$existing
    fi
    [ -n "$owner" ] || {
        printf 'Error: no safe owner is available for: %s\n' "$target" >&2
        return 1
    }
    printf '%s\n' "$owner"
}

apply_owner() {
    root=$1
    owner=$2
    numeric_owner "$owner" >/dev/null || return 1
    find "$root" -exec chown "$owner" {} + || return 1
}

# Transaction files and stale pre-update copies are not committed consumer
# state: config_store, auth, and tls write `<path>.tmp` before a rename, and
# config_write_atomic keeps the previous bytes of the file behind a hard-linked
# `<path>.bak`, so a `.bak` of web-config.json or users is a durable stale copy
# of account state or credentials. Both classes can appear anywhere inside
# either captured tree, so they stay a name class. The manifest names both
# trees and all three suffixes for that class, so a consumer auditing an
# archive can tell the omission from an incomplete backup.
prune_transaction_files() {
    root=$1
    find "$root" -type f \( -name '*.tmp' -o -name '*.new' -o -name '*.bak' \) \
        -exec rm -f {} + || return 1
}

# These are fixed paths directly under the active config directory, not a name
# class: the waked raw PCM dump and the one-shot request that produces it,
# which waked clears as it reads the dump and which would otherwise replay a
# diagnostic outage after a restore, plus the next-boot vendor-import marker.
# They are pruned by exact path, so a file elsewhere in a captured tree that
# merely shares a basename is still backed up, and by path rather than by type,
# so a directory standing at one of those paths is dropped with its contents.
prune_one_shot_config_files() {
    root=$1
    for path in "$root/wake-dump.raw" "$root/wake-dump-seconds" \
        "$root/vendor-import-force-next-boot"; do
        [ ! -e "$path" ] || rm -rf "$path" || return 1
    done
}

# The exclusions are a property of the contract, not of the archive: create
# applies them to what it stages, and every restore applies the same three
# rules to what it unpacked, whatever the archive's manifest version says and
# even when that manifest names no exclusions at all. Pruning the extracted
# trees before the prompt means the prompt describes exactly what will be
# installed, and the required state is re-checked afterwards, so a pruned
# archive cannot lose required state without the restore failing.
apply_restore_exclusions() {
    # Not `root`: the prune helpers assign that name, and the extracted trees
    # are addressed again after they return.
    trees=$1
    [ -d "$trees/config" ] || return 1
    [ -d "$trees/secrets" ] || return 1
    prune_transaction_files "$trees/config" || return 1
    prune_transaction_files "$trees/secrets" || return 1
    prune_one_shot_config_files "$trees/config" || return 1
    [ -f "$trees/config/web-config.json" ] || return 1
    [ -f "$trees/config/users" ] || return 1
    return 0
}

# Printed by `restore` before the prompt and by `list` next to the manifest, so
# the omission is stated rather than silent. Never printed with captured bytes.
report_restore_exclusions() {
    printf '%s\n' \
        'Excluded by contract, never restored: transaction files (*.tmp, *.new, *.bak) and one-shot wake or vendor markers'
}

write_manifest() {
    manifest=$1
    timestamp=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    cat >"$manifest" <<EOF
{
  "version": $BACKUP_VERSION,
  "scope": "active-persistent-state",
  "complete": true,
  "consistency": "file-copy; requires quiescent writers, not an atomic snapshot",
  "timestamp": "$timestamp",
  "components": ["config", "secrets"],
  "required": ["config/web-config.json", "config/users"],
  "secret_policy": "included-with-private-permissions; protect or encrypt archive out-of-band",
  "excluded": ["factory-seed:/etc/libreecho", "payloads:/data/libreecho/features", "ota:/data/libreecho/update", "release-identity:/data/libreecho/data-manifest.json", "runtime-guard:/data/libreecho/network-recovery-reboot.guard", "runtime:/run/libreecho", "logs:/var/log/libreecho", "transaction-files:config,secrets:*.tmp,*.new,*.bak", "config/wake-dump.raw", "config/wake-dump-seconds", "config/vendor-import-force-next-boot", "symlinked-state"]
}
EOF
}

create_backup() {
    output=${1:-/tmp/libreecho-backup-$(date +%Y%m%d-%H%M%S).tar.gz}
    tmpdir=$(mktemp -d)
    trap 'cleanup_dir "$tmpdir"' EXIT HUP INT TERM

    # Refuse a root that is not a plain directory before anything else looks at
    # the state, so the refusal does not depend on which files happen to exist
    # under another spelling of the root.
    refuse_linked_roots ||
        fail "a persistent state root is not a plain directory and cannot be archived"
    require_active_state
    mkdir -p "$tmpdir/persistent/config" "$tmpdir/persistent/secrets"
    copy_tree "$CONFIG_DIR" "$tmpdir/persistent/config"
    copy_tree "$SECRETS_DIR" "$tmpdir/persistent/secrets"
    refuse_symlinks "$tmpdir/persistent/config" ||
        fail "active config contains a symbolic link and cannot be archived"
    refuse_symlinks "$tmpdir/persistent/secrets" ||
        fail "active secrets contain a symbolic link and cannot be archived"
    prune_transaction_files "$tmpdir/persistent/config"
    prune_transaction_files "$tmpdir/persistent/secrets"
    prune_one_shot_config_files "$tmpdir/persistent/config"
    secure_tree "$tmpdir/persistent"
    [ -f "$tmpdir/persistent/config/web-config.json" ] || fail "required active config could not be staged"
    [ -f "$tmpdir/persistent/config/users" ] || fail "required account state could not be staged"
    write_manifest "$tmpdir/manifest.json"

    mkdir -p "$(dirname "$output")"
    tar -czf "$output" -C "$tmpdir" manifest.json persistent
    chmod 600 "$output"
    [ -s "$output" ] || fail "backup archive was not created"
    printf 'Active persistent-state backup created: %s\n' "$output"
    printf 'Scope: config and accounts, supported daemon stores, and private secrets\n'
}

service_script() {
    service=$1
    for candidate in "$SERVICE_DIR/$service.init" "$SERVICE_DIR/$service"; do
        if [ -x "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

stop_services() {
    [ "$SKIP_SERVICES" = 1 ] && return 0
    for service in $SERVICE_NAMES; do
        script=$(service_script "$service") || continue
        service_status=0
        "$script" status >/dev/null 2>&1 || service_status=$?
        case "$service_status" in
            0) ;;
            1|3) continue ;;
            *) printf 'Error: service status unknown: %s\n' "$service" >&2; return 1 ;;
        esac
        # Include the current service in recovery even if its stop fails midway.
        STOPPED_SERVICES="$service $STOPPED_SERVICES"
        "$script" stop >/dev/null 2>&1 || return 1
        service_status=0
        "$script" status >/dev/null 2>&1 || service_status=$?
        case "$service_status" in
            1|3) ;;
            *) return 1 ;;
        esac
    done
    return 0
}

start_services() {
    [ "$SKIP_SERVICES" = 1 ] && return 0
    start_failed=0
    for service in $STOPPED_SERVICES; do
        script=$(service_script "$service") || { start_failed=1; continue; }
        if ! "$script" start >/dev/null 2>&1 || ! "$script" status >/dev/null 2>&1; then
            printf 'Error: could not resume service: %s\n' "$service" >&2
            start_failed=1
        fi
    done
    return "$start_failed"
}

# An archive is untrusted input that this tool reads with root privileges. The
# manifest is the first thing read, and `list` prints it, so it is checked
# before anything opens it: a manifest.json that is a link is a disclosure
# primitive, because reading it would print whatever it points at, and a
# manifest that is not a regular file is not a backup. Every top-level
# component the tool reads is checked the same way, before a probe follows any
# of them.
require_plain_manifest() {
    manifest=$1
    [ ! -L "$manifest" ] || fail "invalid backup (manifest is a symbolic link)"
    [ ! -e "$manifest" ] || [ -f "$manifest" ] ||
        fail "invalid backup (manifest is not a regular file)"
    [ -f "$manifest" ] || fail "invalid backup (missing manifest)"
}

require_plain_dir() {
    directory=$1
    description=$2
    [ ! -L "$directory" ] || fail "invalid backup ($description is a symbolic link)"
    [ -d "$directory" ] || fail "invalid backup (missing $description)"
}

require_required_file() {
    file=$1
    description=$2
    [ ! -L "$file" ] || fail "invalid backup ($description is a symbolic link)"
    [ -f "$file" ] || fail "backup is missing $description"
}

validate_archive() {
    root=$1
    require_plain_manifest "$root/manifest.json"
    require_plain_dir "$root/persistent" 'persistent state component'
    require_plain_dir "$root/persistent/config" 'active config component'
    require_plain_dir "$root/persistent/secrets" 'secrets component'
    require_required_file "$root/persistent/config/web-config.json" 'required active config'
    require_required_file "$root/persistent/config/users" 'required account state'
}

# Extraction is only safe while every member name stays inside the extraction
# root. An absolute name, or one with a `..` component, does not: whether the
# extractor writes it outside or rewrites it is up to the tar implementation
# (GNU tar only strips a leading `/`), and a write outside the staging
# directory would happen as root before any check that runs after extraction
# could see it. The member list is inspected first and the archive is refused,
# so an archive that names a member outside the extraction root is never
# extracted, by restore or by list.
refuse_archive_escape() {
    archive=$1
    listing=$(mktemp) || return 1
    if ! tar -tzf "$archive" >"$listing"; then
        rm -f "$listing"
        printf '%s\n' 'Error: backup archive could not be read' >&2
        return 1
    fi
    escaped=
    while IFS= read -r member; do
        case "$member" in
            /*)
                escaped=$member
                break
                ;;
        esac
        remainder=$member
        while [ -n "$remainder" ]; do
            component=${remainder%%/*}
            case "$component" in
                ..)
                    escaped=$member
                    break 2
                    ;;
            esac
            case "$remainder" in
                */*) remainder=${remainder#*/} ;;
                *) remainder= ;;
            esac
        done
    done <"$listing"
    rm -f "$listing"
    [ -z "$escaped" ] || {
        printf 'Error: backup member escapes the archive root: %s\n' "$escaped" >&2
        return 1
    }
    return 0
}

stage_tree() {
    source=$1
    target=$2
    owner=$3
    # Explicit checks are necessary: callers use this function in a conditional,
    # where POSIX shells suppress errexit even inside called functions.
    [ -d "$source" ] || return 1
    mkdir -p "$target" || return 1
    cp -R "$source/." "$target/" || return 1
    secure_tree "$target" || return 1
    apply_owner "$target" "$owner" || return 1
}

stage_restore_trees() {
    config_owner=$(target_owner "$CONFIG_DIR" "$CONFIG_OWNER") || return 1
    secrets_owner=$(target_owner "$SECRETS_DIR" "$SECRETS_OWNER") || return 1
    STAGED_CONFIG=$(mktemp -d "$CONFIG_DIR.restore-stage.XXXXXX") || return 1
    if ! stage_tree "$1" "$STAGED_CONFIG" "$config_owner"; then
        cleanup_dir "$STAGED_CONFIG"
        STAGED_CONFIG=
        return 1
    fi
    STAGED_SECRETS=$(mktemp -d "$SECRETS_DIR.restore-stage.XXXXXX") || {
        cleanup_dir "$STAGED_CONFIG"
        STAGED_CONFIG=
        return 1
    }
    if ! stage_tree "$2" "$STAGED_SECRETS" "$secrets_owner"; then
        cleanup_dir "$STAGED_CONFIG"
        cleanup_dir "$STAGED_SECRETS"
        STAGED_CONFIG=
        STAGED_SECRETS=
        return 1
    fi
}

restore_original_trees() {
    config_backup=$1
    secrets_backup=$2
    rollback_failed=0
    [ ! -e "$CONFIG_DIR" ] || rm -rf "$CONFIG_DIR" || rollback_failed=1
    [ ! -e "$SECRETS_DIR" ] || rm -rf "$SECRETS_DIR" || rollback_failed=1
    [ -e "$config_backup" ] && mv "$config_backup" "$CONFIG_DIR" || rollback_failed=1
    [ -e "$secrets_backup" ] && mv "$secrets_backup" "$SECRETS_DIR" || rollback_failed=1
    return "$rollback_failed"
}

replace_staged_trees() {
    config_backup=$(mktemp -d "$CONFIG_DIR.restore-backup.XXXXXX") || return 1
    rmdir "$config_backup" || return 1
    secrets_backup=$(mktemp -d "$SECRETS_DIR.restore-backup.XXXXXX") || {
        cleanup_dir "$config_backup"
        return 1
    }
    rmdir "$secrets_backup" || {
        cleanup_dir "$config_backup"
        cleanup_dir "$secrets_backup"
        return 1
    }
    if ! mv "$CONFIG_DIR" "$config_backup"; then
        cleanup_dir "$config_backup"
        cleanup_dir "$secrets_backup"
        return 1
    fi
    if ! mv "$SECRETS_DIR" "$secrets_backup"; then
        if ! mv "$config_backup" "$CONFIG_DIR"; then
            printf 'Error: config rollback failed; saved original retained at %s\n' \
                "$config_backup" >&2
        fi
        # Never delete an original tree when its recovery rename failed.
        return 1
    fi
    if ! mv "$STAGED_CONFIG" "$CONFIG_DIR"; then
        if ! restore_original_trees "$config_backup" "$secrets_backup"; then
            printf '%s\n' 'Error: failed to roll back staged config replacement' >&2
        fi
        return 1
    fi
    STAGED_CONFIG=
    if ! mv "$STAGED_SECRETS" "$SECRETS_DIR"; then
        if ! restore_original_trees "$config_backup" "$secrets_backup"; then
            printf '%s\n' 'Error: failed to roll back staged secrets replacement' >&2
        fi
        return 1
    fi
    STAGED_SECRETS=
    rm -rf "$config_backup" || return 1
    rm -rf "$secrets_backup" || return 1
}

restore_backup() {
    backup=$1
    [ -f "$backup" ] || fail "backup file not found: $backup"
    refuse_archive_escape "$backup" ||
        fail "backup members can escape the archive root; nothing was changed"
    tmpdir=$(mktemp -d)
    trap 'cleanup_dir "$tmpdir"' EXIT HUP INT TERM
    tar -xzf "$backup" -C "$tmpdir"
    validate_archive "$tmpdir"
    refuse_symlinks "$tmpdir/persistent" ||
        fail "backup contains a symbolic link in persistent state; nothing was changed"
    apply_restore_exclusions "$tmpdir/persistent" ||
        fail "backup exclusions could not be applied; nothing was changed"
    refuse_linked_roots ||
        fail "a persistent state root is not a plain directory; state was not changed"

    report_restore_exclusions
    printf 'Restore active persistent state from %s? (y/N) ' "$backup"
    reply=
    IFS= read -r reply || true
    case "$reply" in
        y|Y) ;;
        *) printf '%s\n' 'Restore cancelled'; exit 0 ;;
    esac

    if ! target_owner "$CONFIG_DIR" "$CONFIG_OWNER" >/dev/null ||
       ! target_owner "$SECRETS_DIR" "$SECRETS_OWNER" >/dev/null; then
        fail "restore owner validation failed; state was not changed"
    fi
    if ! stop_services; then
        start_services || printf '%s\n' 'Warning: manual service recovery required' >&2
        fail "could not quiesce services; persistent state was not changed"
    fi
    if ! stage_restore_trees "$tmpdir/persistent/config" "$tmpdir/persistent/secrets"; then
        fail "restore staging failed; state was not changed; services remain stopped for recovery"
    fi
    if ! replace_staged_trees; then
        cleanup_dir "$STAGED_CONFIG"
        cleanup_dir "$STAGED_SECRETS"
        fail "restore replacement failed; services remain stopped for recovery"
    fi
    sync || fail "restore sync failed; services remain stopped for recovery"
    start_services || fail "state restored but service recovery is incomplete"
    printf '%s\n' 'Active persistent-state restore complete'
}

list_backup() {
    backup=$1
    [ -f "$backup" ] || fail "backup file not found: $backup"
    refuse_archive_escape "$backup" ||
        fail "backup members can escape the archive root and were not listed"
    tmpdir=$(mktemp -d)
    trap 'cleanup_dir "$tmpdir"' EXIT HUP INT TERM
    tar -xzf "$backup" -C "$tmpdir"
    validate_archive "$tmpdir"
    refuse_symlinks "$tmpdir/persistent" ||
        fail "backup contains a symbolic link in persistent state and was not listed"
    printf 'Backup: %s\nManifest:\n' "$backup"
    cat "$tmpdir/manifest.json"
    report_restore_exclusions
    printf '%s\n' 'Contents:'
    tar -tzf "$backup"
}

case "${1:-}" in
    create) create_backup "${2:-}" ;;
    restore)
        [ -n "${2:-}" ] || usage
        restore_backup "$2"
        ;;
    list)
        [ -n "${2:-}" ] || usage
        list_backup "$2"
        ;;
    *) usage ;;
esac
