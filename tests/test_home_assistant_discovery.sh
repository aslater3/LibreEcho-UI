#!/bin/sh
set -eu

SCRIPT=init/libreecho-airplayd.init
MDNS_SCRIPT=init/libreecho-mdnsd.init
SERVICE=config/wyoming.service

sh -n "$SCRIPT"
sh -n "$MDNS_SCRIPT"
test -f "$SERVICE"
grep -Fq '<type>_wyoming._tcp</type>' "$SERVICE"
grep -Fq '<port>10700</port>' "$SERVICE"

# Discovery is owned by the shared libreecho-mdnsd supervisor. The Wyoming
# record is rendered and staged by that service, not by the AirPlay controller.
grep -Fq 'HOME_ASSISTANT_ENABLED' "$MDNS_SCRIPT"
grep -Fq 'integrations & 1' "$MDNS_SCRIPT"
grep -Fq 'WYOMING_SERVICE_SOURCE=${WYOMING_SERVICE_SOURCE:-/etc/libreecho/avahi-services/wyoming.service}' "$MDNS_SCRIPT"
# The advertised port follows the effective Wyoming PORT override, not the
# built-in default, so the service is rendered rather than copied verbatim.
grep -Fq 'WYOMING_DEFAULTS=${WYOMING_DEFAULTS:-/etc/default/libreecho-wyomingd}' "$MDNS_SCRIPT"
grep -Fq 'port=$(wyoming_service_port)' "$MDNS_SCRIPT"
grep -Fq 'sed "s#<port>[0-9][0-9]*</port>#<port>$port</port>#' "$MDNS_SCRIPT"
# The advertised port must follow an explicit --port in the ARGS override too,
# which the daemon passes verbatim in preference to PORT.
grep -Fq -- '--port[[:space:]=]' "$MDNS_SCRIPT"
# An explicit pipeline mode is a second signal for the same advertisement:
# selecting local/custom stops Wyoming, so the record must be withdrawn.
grep -Fq 'home_assistant_voice_mode' "$MDNS_SCRIPT"
# The AirPlay controller stages its own AirPlay service definitions into the
# shared services directory and otherwise treats the supervisor as external.
grep -Fq 'MDNS_SERVICES_DIR=${MDNS_SERVICES_DIR:-/usr/local/lib/libreecho-mdns/root/etc/avahi/services}' "$SCRIPT"
grep -Fq 'cp -a "$AVAHI_SERVICES_SOURCE/." "$MDNS_SERVICES_DIR/"' "$SCRIPT"

# Discovery refresh must be an independent step, not a link in the pipeline
# `||` chain that a failing init script can break.
grep -Fq '#define LE_INIT_MDNSD     "/etc/init.d/libreecho-mdnsd.init"' src/api.c
grep -Fq 'refresh_home_assistant_discovery' src/api.c
# The supervisor result must be recorded and reported when the Wyoming
# advertisement cannot be refreshed, without failing the pipeline transition.
grep -Fq 'home_assistant_discovery_unavailable' src/api.c
# The pipeline mode and the Home Assistant integration bit are two signals for
# the same Wyoming daemon, so they must stay synchronized: selecting or leaving
# a non-Home-Assistant engine clears the bit the supervisor advertises from,
# and disabling the integration clears a persisted Home Assistant pipeline mode
# instead of leaving the advertisement pointing at a stopped daemon.
grep -Fq 'c->integrations &= ~1u;' src/api.c
grep -Fq 'c->integrations&=~1u;' src/api.c
# Restoration belongs to the post-handler lifecycle, not an inline reset
# that would destroy the previous custom/local selection before it runs.
grep -Fq 'after_integration_change' src/api.c
grep -Fq 'voice_pipeline_previous_mode' src/api.c

printf '%s\n' 'Home Assistant Wyoming discovery lifecycle: ok'
