#!/bin/sh
# LibreEcho backup/restore tool
# Creates and restores complete system backups

set -e

BACKUP_VERSION=1
CONFIG_DIR=/etc/libreecho
LOG_DIR=/var/log/libreecho
WEB_ROOT=/usr/local/share/libreecho/web

usage() {
    cat <<EOF
Usage: $0 {create|restore|list} [path]

Commands:
  create [path]    Create backup (default: /tmp/libreecho-backup-<timestamp>.tar.gz)
  restore <path>   Restore from backup
  list <path>      List backup contents

Examples:
  $0 create
  $0 create /tmp/my-backup.tar.gz
  $0 restore /tmp/libreecho-backup-20260720-143022.tar.gz
  $0 list /tmp/libreecho-backup-20260720-143022.tar.gz
EOF
    exit 1
}

create_backup() {
    local output="${1:-/tmp/libreecho-backup-$(date +%Y%m%d-%H%M%S).tar.gz}"
    local tmpdir
    local manifest
    
    tmpdir=$(mktemp -d)
    trap "rm -rf '$tmpdir'" EXIT
    
    echo "Creating backup..."
    
    # Create manifest
    manifest="$tmpdir/manifest.json"
    cat > "$manifest" <<EOF
{
  "version": $BACKUP_VERSION,
  "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "hostname": "$(hostname)",
  "components": [
    "config",
    "logs",
    "web-state"
  ]
}
EOF
    
    # Copy config files
    if [ -d "$CONFIG_DIR" ]; then
        mkdir -p "$tmpdir/config"
        cp -a "$CONFIG_DIR"/* "$tmpdir/config/" 2>/dev/null || true
        echo "  ✓ Config files"
    fi
    
    # Copy recent logs (last 7 days)
    if [ -d "$LOG_DIR" ]; then
        mkdir -p "$tmpdir/logs"
        find "$LOG_DIR" -name "*.log*" -mtime -7 -exec cp -a {} "$tmpdir/logs/" \; 2>/dev/null || true
        echo "  ✓ Recent logs"
    fi
    
    # Copy web daemon state
    if [ -f /var/lib/libreecho/web-state.json ]; then
        mkdir -p "$tmpdir/web"
        cp -a /var/lib/libreecho/web-state.json "$tmpdir/web/" 2>/dev/null || true
        echo "  ✓ Web state"
    fi
    
    # Create tarball
    tar -czf "$output" -C "$tmpdir" .
    
    echo ""
    echo "Backup created: $output"
    echo "Size: $(du -h "$output" | cut -f1)"
}

restore_backup() {
    local backup="$1"
    local tmpdir
    
    if [ ! -f "$backup" ]; then
        echo "Error: backup file not found: $backup" >&2
        exit 1
    fi
    
    tmpdir=$(mktemp -d)
    trap "rm -rf '$tmpdir'" EXIT
    
    echo "Extracting backup..."
    tar -xzf "$backup" -C "$tmpdir"
    
    # Verify manifest
    if [ ! -f "$tmpdir/manifest.json" ]; then
        echo "Error: invalid backup (missing manifest)" >&2
        exit 1
    fi
    
    echo "Backup contents:"
    cat "$tmpdir/manifest.json"
    echo ""
    
    read -p "Restore this backup? (y/N) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "Restore cancelled"
        exit 0
    fi
    
    echo "Restoring..."
    
    # Stop services
    echo "  Stopping services..."
    /etc/init.d/libreecho-web stop 2>/dev/null || true
    /etc/init.d/libreecho-networkd stop 2>/dev/null || true
    /etc/init.d/libreecho-audiod stop 2>/dev/null || true
    /etc/init.d/libreecho-ledd stop 2>/dev/null || true
    /etc/init.d/libreecho-logd stop 2>/dev/null || true
    sleep 2
    
    # Restore config
    if [ -d "$tmpdir/config" ]; then
        echo "  Restoring config..."
        mkdir -p "$CONFIG_DIR"
        cp -a "$tmpdir/config"/* "$CONFIG_DIR/" 2>/dev/null || true
    fi
    
    # Restore logs (optional, ask user)
    if [ -d "$tmpdir/logs" ]; then
        read -p "  Restore logs? (y/N) " -n 1 -r
        echo
        if [[ $REPLY =~ ^[Yy]$ ]]; then
            mkdir -p "$LOG_DIR"
            cp -a "$tmpdir/logs"/* "$LOG_DIR/" 2>/dev/null || true
        fi
    fi
    
    # Restore web state
    if [ -d "$tmpdir/web" ]; then
        echo "  Restoring web state..."
        mkdir -p /var/lib/libreecho
        cp -a "$tmpdir/web"/* /var/lib/libreecho/ 2>/dev/null || true
    fi
    
    # Start services
    echo "  Starting services..."
    /etc/init.d/libreecho-logd start 2>/dev/null || true
    sleep 1
    /etc/init.d/libreecho-networkd start 2>/dev/null || true
    /etc/init.d/libreecho-audiod start 2>/dev/null || true
    /etc/init.d/libreecho-ledd start 2>/dev/null || true
    /etc/init.d/libreecho-web start 2>/dev/null || true
    
    echo ""
    echo "Restore complete"
}

list_backup() {
    local backup="$1"
    local tmpdir
    
    if [ ! -f "$backup" ]; then
        echo "Error: backup file not found: $backup" >&2
        exit 1
    fi
    
    tmpdir=$(mktemp -d)
    trap "rm -rf '$tmpdir'" EXIT
    
    tar -xzf "$backup" -C "$tmpdir"
    
    echo "Backup: $backup"
    echo ""
    
    if [ -f "$tmpdir/manifest.json" ]; then
        echo "Manifest:"
        cat "$tmpdir/manifest.json"
        echo ""
    fi
    
    echo "Contents:"
    find "$tmpdir" -type f | sed "s|$tmpdir/||" | sort
}

# Main
case "${1:-}" in
    create)
        create_backup "${2:-}"
        ;;
    restore)
        if [ -z "${2:-}" ]; then
            echo "Error: restore requires a backup path" >&2
            usage
        fi
        restore_backup "$2"
        ;;
    list)
        if [ -z "${2:-}" ]; then
            echo "Error: list requires a backup path" >&2
            usage
        fi
        list_backup "$2"
        ;;
    *)
        usage
        ;;
esac
