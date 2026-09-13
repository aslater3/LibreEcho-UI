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
# The advertised port must follow an explicit --port in the ARGS override too,
# which the daemon passes verbatim in preference to PORT.
grep -Fq -- '--port[[:space:]=]' "$SCRIPT"
# Discovery refresh must be an independent step, not a link in the pipeline
# `||` chain that the AirPlay controller's optional-payload failure can break.
grep -Fq '#define LE_INIT_AIRPLAYD  "/etc/init.d/libreecho-airplayd.init"' src/api.c
grep -Fq 'refresh_home_assistant_discovery' src/api.c
# The controller result must be recorded and reported when the Wyoming
# advertisement cannot be refreshed, without failing the pipeline transition.
grep -Fq 'home_assistant_discovery_unavailable' src/api.c
# The documented voice-pipeline route selects Home Assistant voice too, so it
# must also synchronize the advertisement.
grep -Fq 'home_assistant_voice_mode' "$SCRIPT"
# The pipeline mode and the Home Assistant integration bit are two signals for
# the same Wyoming daemon, so they must stay synchronized: selecting or leaving
# a non-Home-Assistant engine clears the bit the controller advertises from,
# and disabling the integration clears a persisted Home Assistant pipeline mode
# instead of leaving the advertisement pointing at a stopped daemon.
grep -Fq 'c->integrations &= ~1u;' src/api.c
grep -Fq 'c->integrations&=~1u;' src/api.c
# Restoration belongs to the post-handler lifecycle, not an inline reset
# that would destroy the previous custom/local selection before it runs.
grep -Fq 'after_integration_change' src/api.c
grep -Fq 'voice_pipeline_previous_mode' src/api.c

printf '%s\n' 'Home Assistant Wyoming discovery lifecycle: ok'
