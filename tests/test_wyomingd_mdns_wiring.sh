#!/bin/sh
# Contract for the shipped shared mDNS wiring: the Wyoming daemon only
# registers when it is actually told where the supervisor socket lives, and it
# must not treat a missing supervisor as a fatal listener error.
set -eu

INIT=init/libreecho-wyomingd.init
SOURCE=src/adapter/wyomingd.c

sh -n "$INIT"

# The supervisor socket is a shared, image-wide path, and the daemon must be
# launched with it: parsing --mdns-socket is not enough if nothing passes it.
grep -Fq 'MDNS_SOCKET=${MDNS_SOCKET:-/run/libreecho/mdns.sock}' "$INIT"
grep -Fq -- '--mdns-socket $MDNS_SOCKET' "$INIT"

# Registration is optional discovery, never a startup precondition: a missing
# or unreachable supervisor must not stop the Wyoming listener.
if grep -Fq 'mdns_ready' "$INIT"; then
    echo "wyomingd init must not gate startup on mDNS readiness" >&2
    exit 1
fi

# The daemon accepts the flag and uses it: parsing is not enough.
grep -Fq '"--mdns-socket"' "$SOURCE"
grep -Fq 'static void mdns_maintain' "$SOURCE"
grep -Fq 'mdns_maintain(mdns_socket, state.port' "$SOURCE"
grep -Fq 'le_mdns_receive(' "$SOURCE"
# A lease must never outlive the process that owns it.
grep -Fq 'mdns_lease_close' "$SOURCE"
grep -Fq 'mdns_lease_close(&mdns_fd)' "$SOURCE"

# Registration happens only after the listener is accepting: the supervisor
# must never be told about a port nothing is listening on.
listen_line=$(grep -n 'listening on TCP port' "$SOURCE" | sed -n '1p' | cut -d: -f1)
mdns_line=$(grep -n 'mdns_maintain(mdns_socket, state.port' "$SOURCE" | sed -n '1p' | cut -d: -f1)
if [ -z "$listen_line" ] || [ -z "$mdns_line" ] || [ "$mdns_line" -le "$listen_line" ]; then
    echo "the Wyoming daemon must register only after its listener is up" >&2
    exit 1
fi

printf '%s\n' 'Wyoming shared mDNS wiring: ok'
