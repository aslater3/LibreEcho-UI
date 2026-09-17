#!/bin/sh
# LibreEcho active persistent-state backup/restore tool.
#
# The factory image under /etc/libreecho is a seed, not live state.  This tool
# deliberately scopes itself to the consumer-owned /data/libreecho config and
# secrets directories.  Feature payloads, OTA state, release identity, logs,
# runtime state, transaction files, one-shot markers, and symlinked state are
# outside this backup contract: they are never captured and never restored.

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
        "The archive contains /data/libreecho/config and /data/libreecho/secrets" \
        "(with transaction files and raw wake diagnostics excluded)."
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
# is any spelling of one that uses a `.` or `..` component.
refuse_linked_roots() {
    for root in "$DATA_ROOT" "$CONFIG_DIR" "$SECRETS_DIR"; do
        if has_dot_component "$root"; then
            printf 'Error: persistent state root uses a dot component: %s\n' "$root" >&2
            return 1
        fi
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

# Transaction files are not committed consumer state, and config_store can
# leave them anywhere inside either captured tree, so they stay a name class.
prune_transaction_files() {
    root=$1
    find "$root" -type f \( -name '*.tmp' -o -name '*.new' \) -exec rm -f {} + || return 1
}

# These are fixed paths directly under the active config directory, not a name
# class: the waked raw PCM dump and the one-shot request that produces it,
# which waked clears as it reads the dump and which would otherwise replay a
# diagnostic outage after a restore, plus the next-boot vendor-import marker.
# They are pruned by exact path so a file elsewhere in a captured tree that
# merely shares a basename is still backed up.
prune_one_shot_config_files() {
    root=$1
    for path in "$root/wake-dump.raw" "$root/wake-dump-seconds" \
        "$root/vendor-import-force-next-boot"; do
        [ ! -f "$path" ] || rm -f "$path" || return 1
    done
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
  "excluded": ["factory-seed:/etc/libreecho", "payloads:/data/libreecho/features", "ota:/data/libreecho/update", "release-identity:/data/libreecho/data-manifest.json", "runtime-guard:/data/libreecho/network-recovery-reboot.guard", "runtime:/run/libreecho", "logs:/var/log/libreecho", "config-transaction-files:config/*.tmp", "config/wake-dump.raw", "config/wake-dump-seconds", "config/vendor-import-force-next-boot", "symlinked-state"]
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

validate_archive() {
    root=$1
    [ -f "$root/manifest.json" ] || fail "invalid backup (missing manifest)"
    [ -d "$root/persistent/config" ] || fail "invalid backup (missing active config component)"
    [ -d "$root/persistent/secrets" ] || fail "invalid backup (missing secrets component)"
    [ -f "$root/persistent/config/web-config.json" ] || fail "backup is missing required active config"
    [ -f "$root/persistent/config/users" ] || fail "backup is missing required account state"
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
    tmpdir=$(mktemp -d)
    trap 'cleanup_dir "$tmpdir"' EXIT HUP INT TERM
    tar -xzf "$backup" -C "$tmpdir"
    validate_archive "$tmpdir"
    refuse_symlinks "$tmpdir/persistent" ||
        fail "backup contains a symbolic link in persistent state; nothing was changed"
    refuse_linked_roots ||
        fail "a persistent state root is not a plain directory; state was not changed"

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
    tmpdir=$(mktemp -d)
    trap 'cleanup_dir "$tmpdir"' EXIT HUP INT TERM
    tar -xzf "$backup" -C "$tmpdir"
    validate_archive "$tmpdir"
    refuse_symlinks "$tmpdir/persistent" ||
        fail "backup contains a symbolic link in persistent state and was not listed"
    printf 'Backup: %s\nManifest:\n' "$backup"
    cat "$tmpdir/manifest.json"
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
