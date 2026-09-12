#!/bin/sh
set -eu

SCRIPT=init/libreecho-airplayd.init
SERVICE=config/wyoming.service

sh -n "$SCRIPT"
test -f "$SERVICE"
grep -Fq '<type>_wyoming._tcp</type>' "$SERVICE"
grep -Fq '<port>10700</port>' "$SERVICE"
grep -Fq 'HOME_ASSISTANT_ENABLED' "$SCRIPT"
grep -Fq 'integrations & 1' "$SCRIPT"
grep -Fq -- '--mdns-on-start' "$SCRIPT"
grep -Fq 'AVAHI_SERVICES_SOURCE=${AVAHI_SERVICES_SOURCE:-/data/libreecho/features/airplay2/avahi-services}' "$SCRIPT"
grep -Fq 'cp -a "$AVAHI_SERVICES_SOURCE/." "$RUNTIME_ROOT/etc/avahi/services/"' "$SCRIPT"
grep -Fq 'WYOMING_SERVICE_SOURCE=${WYOMING_SERVICE_SOURCE:-/etc/libreecho/avahi-services/wyoming.service}' "$SCRIPT"
grep -Fq 'cp "$WYOMING_SERVICE_SOURCE" "$RUNTIME_ROOT/etc/avahi/services/wyoming.service"' "$SCRIPT"
grep -Fq 'libreecho-airplayd.init", "restart"' src/api.c

printf '%s\n' 'Home Assistant Wyoming discovery lifecycle: ok'
