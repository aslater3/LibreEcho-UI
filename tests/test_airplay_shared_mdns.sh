#!/bin/sh
# The AirPlay controller must not own D-Bus or Avahi any more: the shared mDNS
# supervisor is an external dependency. AirPlay audio/Shairport lifecycle stays
# with the controller.
set -eu

SCRIPT=init/libreecho-airplayd.init
SOURCE=src/adapter/airplayd.c

sh -n "$SCRIPT"

# No private discovery stack: neither process management nor its mounts.
if grep -Eq 'dbus-daemon|avahi-daemon' "$SCRIPT"; then
    echo "airplayd init must not start or stop D-Bus/Avahi daemons" >&2
    exit 1
fi
if grep -Fq -- '--mdns-on-start' "$SCRIPT"; then
    echo "airplayd must not own the mDNS advertisement lifecycle" >&2
    exit 1
fi
if grep -Eq 'run/avahi-daemon|var/lib/avahi|libreecho-airplay-avahi|prepare_avahi_runtime' "$SCRIPT"; then
    echo "airplayd init must not prepare an Avahi runtime" >&2
    exit 1
fi
if grep -Eq 'spawn_dbus|spawn_avahi|"dbus-daemon"|"avahi-daemon"' "$SOURCE"; then
    echo "airplayd must not spawn D-Bus/Avahi children" >&2
    exit 1
fi

# The shared supervisor is a declared, external dependency.
grep -Fq 'MDNS_INIT=${MDNS_INIT:-/etc/init.d/libreecho-mdnsd.init}' "$SCRIPT"
grep -Fq 'MDNS_SOCKET=${MDNS_SOCKET:-/run/libreecho/mdns.sock}' "$SCRIPT"
grep -Fq 'MDNS_SERVICES_DIR=${MDNS_SERVICES_DIR:-/usr/local/lib/libreecho-mdns/root/etc/avahi/services}' "$SCRIPT"
grep -Fq 'MDNS_BUS_SOURCE=${MDNS_BUS_SOURCE:-/usr/local/lib/libreecho-mdns/root/run/dbus/system_bus_socket}' "$SCRIPT"
grep -Fq 'mount_shared_mdns_bus()' "$SCRIPT"
grep -Fq 'mount --bind "$MDNS_BUS_SOURCE" "$target_socket"' "$SCRIPT"
grep -Fq 'mdns_status()' "$SCRIPT"
grep -Fq -e '"$MDNS_INIT" status' "$SCRIPT"
grep -Fq 'le_mdns_status(' "$SOURCE"
grep -Fq 'LE_MDNS_BUS_SOCKET' "$SOURCE"
grep -Fq 'mdns_running' "$SOURCE"

# AirPlay audio and Shairport lifecycle is unchanged and still owned here.
grep -Fq 'shairport_path' "$SOURCE"
grep -Fq 'nqptp_path' "$SOURCE"
grep -Fq 'metadata_fifo_open' "$SOURCE"
grep -Fq 'ARGS="$ARGS --enable-on-start"' "$SCRIPT"
grep -Fq 'integrations & 16' "$SCRIPT"
grep -Fq 'source_socket=/run/libreecho/led.sock' "$SCRIPT"

# The controller may not sweep unrelated processes.
if grep -Fq 'killall' "$SCRIPT" "$SOURCE"; then
    echo "AirPlay lifecycle must not use a global process sweep" >&2
    exit 1
fi

# The announced name must come from this device's own configuration: the payload
# ships a fixed name, so two units on one LAN announce the same _airplay._tcp
# name and Avahi withholds or renames one of them. Only the RAOP record survives
# because its instance name is MAC-prefixed, which is why such a unit is absent
# from AirPlay pickers while still visible as a legacy speaker.
grep -Fq 'configure_service_name()' "$SCRIPT"
grep -Fq '"device_name"' "$SCRIPT"
grep -Fq 'mount_support "$target" --bind "$generated"' "$SCRIPT"
grep -Fq 'name = \"[^/\"]*\"' "$SCRIPT"

# Behavioural check of that substitution, on a scratch runtime: the general
# section's name is replaced and the pipe/metadata names are left alone.
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/root/etc/libreecho" "$work/root/var"
cat >"$work/root/etc/libreecho/airplay2.conf" <<'CONF'
general = {
    name = "LibreEcho";
    service_type = "airplay2";
};
pipe = {
    name = "/run/libreecho/airplay.pcm";
};
metadata = {
    pipe_name = "/run/libreecho-audio/airplay.metadata";
};
CONF
printf '{"device_name": "libreecho-dot"}\n' >"$work/config.json"
sed -n '/^configure_service_name()/,/^}/p' "$SCRIPT" >"$work/function.sh"
[ -s "$work/function.sh" ] || { echo "configure_service_name was not extractable" >&2; exit 1; }

RUNTIME_ROOT="$work/root"
CONFIG="$work/config.json"
# The mount is stubbed: the assertion is about the generated file, and a real
# bind mount needs privileges this check must not require.
mount_support() { stub_mount_target=$1; }
. "$work/function.sh"
configure_service_name

generated="$work/root/var/libreecho-airplay2.conf"
[ -f "$generated" ] || { echo "no name override was generated" >&2; exit 1; }
[ "$stub_mount_target" = "$work/root/etc/libreecho/airplay2.conf" ] || {
    echo "the override was not bound over the payload config" >&2; exit 1; }
grep -Fq 'name = "libreecho-dot";' "$generated"
grep -Fq 'name = "/run/libreecho/airplay.pcm";' "$generated"
grep -Fq 'pipe_name = "/run/libreecho-audio/airplay.metadata";' "$generated"
# The payload name must not survive twice, and nothing else may be rewritten:
# count only section-level names, since "pipe_name" contains "name = ".
[ "$(grep -c '^[[:space:]]*name = "' "$generated")" -eq 2 ] || {
    echo "the substitution changed more than the announced name" >&2; exit 1; }

# A name that is not a plain service name is refused rather than written into
# the config, and the payload file is left in place. The fallback source is
# asserted statically: it is the kernel host name.
grep -Fq 'cat /etc/hostname' "$SCRIPT"
rm -f "$generated"
printf '{"device_name": "bad\\"name; x"}\n' >"$work/config-bad.json"
sh -c 'RUNTIME_ROOT=$1; CONFIG=$2; mount_support() { :; }; . "$3"; configure_service_name' \
    sh "$work/root" "$work/config-bad.json" "$work/function.sh" || true
[ ! -f "$generated" ] || { echo "an unsafe service name was accepted" >&2; exit 1; }

printf '%s\n' 'AirPlay external shared mDNS dependency: ok'
