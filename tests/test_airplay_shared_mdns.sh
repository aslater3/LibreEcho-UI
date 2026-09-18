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

printf '%s\n' 'AirPlay external shared mDNS dependency: ok'
