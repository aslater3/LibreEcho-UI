#!/bin/sh
# Shared discovery contract; HA registration belongs only to the live ESPHome
# owner. AirPlay still treats the responder as an external shared dependency.
set -eu
SCRIPT=init/libreecho-airplayd.init
MDNS_SCRIPT=init/libreecho-mdnsd.init
SERVICE=config/esphome.service
sh -n "$SCRIPT"
sh -n "$MDNS_SCRIPT"
test -f "$SERVICE"
grep -Fq '<type>_esphomelib._tcp</type>' "$SERVICE"
grep -Fq '<port>6053</port>' "$SERVICE"
grep -Fq 'api_encryption=Noise_NNpsk0_25519_ChaChaPoly_SHA256' "$SERVICE"
grep -Fq '#define MDNS_OWNER_EXE "/usr/local/sbin/libreecho-esphomed"' src/adapter/mdnsd.c
if grep -Eq '_wyoming\._tcp|render_wyoming_service|WYOMING_SERVICE_SOURCE' "$MDNS_SCRIPT" src/adapter/mdns_lease.c src/adapter/mdnsd.c; then
    echo 'shared discovery must not publish Wyoming/static HA records' >&2
    exit 1
fi
grep -Fq 'clear_ha_services' "$MDNS_SCRIPT"
grep -Fq 'MDNS_SERVICES_DIR=${MDNS_SERVICES_DIR:-/usr/local/lib/libreecho-mdns/root/etc/avahi/services}' "$SCRIPT"
grep -Fq 'cp -a "$AVAHI_SERVICES_SOURCE/." "$MDNS_SERVICES_DIR/"' "$SCRIPT"
# Behavioral XML/schema and isolated cold-boot/config-toggle cleanup coverage.
python3 tests/test_wyoming_discovery_port.py
printf '%s\n' 'Home Assistant ESPHome lease-only discovery lifecycle: ok'
