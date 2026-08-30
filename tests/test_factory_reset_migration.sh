#!/bin/sh
set -eu
root=$(mktemp -d /tmp/libreecho-web-migrate.XXXXXX)
trap 'rm -rf "$root"' EXIT INT TERM
legacy=$root/legacy
data=$root/data
mkdir -p "$legacy" "$data/config"
printf 'legacy-config\n' > "$legacy/web-config.json"
printf 'complete\n' > "$legacy/web-config.json.setup-complete"
printf 'legacy-users\n' > "$legacy/users"
printf 'keep-current\n' > "$data/config/web-config.json"
LIBREECHO_LEGACY_CONFIG_ROOT="$legacy" \
LIBREECHO_DATA_ROOT="$data" \
LIBREECHO_STATE_OWNER="$(id -un)" \
LIBREECHO_STATE_GROUP="$(id -gn)" \
    sh init/libreecho-web-migrate-state
[ "$(cat "$data/config/web-config.json")" = 'keep-current' ]
[ "$(cat "$data/config/web-config.json.setup-complete")" = 'complete' ]
[ "$(cat "$data/config/users")" = 'legacy-users' ]
[ -d "$data/secrets" ]
[ "$(stat -c %a "$data" "$data/config" "$data/secrets" | sort -u)" = '700' ]
printf 'changed\n' > "$legacy/users"
LIBREECHO_LEGACY_CONFIG_ROOT="$legacy" \
LIBREECHO_DATA_ROOT="$data" \
LIBREECHO_STATE_OWNER="$(id -un)" \
LIBREECHO_STATE_GROUP="$(id -gn)" \
    sh init/libreecho-web-migrate-state
[ "$(cat "$data/config/users")" = 'legacy-users' ]
echo 'systemd legacy state migration is non-destructive: ok'
