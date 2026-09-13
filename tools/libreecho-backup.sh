#!/bin/sh
# LibreEcho active persistent-state backup/restore tool.
#
# The factory image under /etc/libreecho is a seed, not live state.  This tool
# deliberately scopes itself to the consumer-owned /data/libreecho config and
# secrets directories.  Feature payloads, OTA state, release identity, logs,
# and runtime state are outside this backup contract.

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
    [ -f "$CONFIG_DIR/web-config.json" ] || fail "required active config is unavailable"
    [ -f "$CONFIG_DIR/users" ] || fail "required account state is unavailable"
    [ -r "$CONFIG_DIR/web-config.json" ] || fail "required active config is unreadable"
    [ -r "$CONFIG_DIR/users" ] || fail "required account state is unreadable"
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

prune_excluded_files() {
    root=$1
    # Transaction files are not committed consumer state.  Raw PCM is a
    # diagnostic artifact and may contain private microphone audio.
    find "$root" -type f \( -name '*.tmp' -o -name '*.new' -o -name 'wake-dump.raw' -o -name 'vendor-import-force-next-boot' \) -exec rm -f {} + || return 1
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
  "excluded": ["factory-seed:/etc/libreecho", "features:/data/libreecho/features", "ota:/data/libreecho/update", "runtime:/run/libreecho", "logs", "config-transaction-files", "config/wake-dump.raw", "config/vendor-import-force-next-boot", "release-identity"]
}
EOF
}

create_backup() {
    output=${1:-/tmp/libreecho-backup-$(date +%Y%m%d-%H%M%S).tar.gz}
    tmpdir=$(mktemp -d)
    trap 'cleanup_dir "$tmpdir"' EXIT HUP INT TERM

    require_active_state
    mkdir -p "$tmpdir/persistent/config" "$tmpdir/persistent/secrets"
    copy_tree "$CONFIG_DIR" "$tmpdir/persistent/config"
    copy_tree "$SECRETS_DIR" "$tmpdir/persistent/secrets"
    prune_excluded_files "$tmpdir/persistent/config"
    prune_excluded_files "$tmpdir/persistent/secrets"
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
