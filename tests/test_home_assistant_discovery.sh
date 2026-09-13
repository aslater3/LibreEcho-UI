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
# The advertised port follows the effective Wyoming PORT override, not the
# built-in default, so the service is rendered rather than copied verbatim.
grep -Fq 'WYOMING_DEFAULTS=${WYOMING_DEFAULTS:-/etc/default/libreecho-wyomingd}' "$SCRIPT"
grep -Fq 'port=$(wyoming_service_port)' "$SCRIPT"
grep -Fq 'sed "s#<port>[0-9][0-9]*</port>#<port>$port</port>#' "$SCRIPT"
# Discovery refresh must be an independent step, not a link in the pipeline
# `||` chain that the AirPlay controller's optional-payload failure can break.
grep -Fq '#define LE_INIT_AIRPLAYD  "/etc/init.d/libreecho-airplayd.init"' src/api.c
grep -Fq 'LE_INIT_AIRPLAYD, "restart", NULL' src/api.c
grep -Fq 'if (access(LE_INIT_AIRPLAYD, X_OK) == 0)' src/api.c

printf '%s\n' 'Home Assistant Wyoming discovery lifecycle: ok'
