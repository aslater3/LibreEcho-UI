#ifndef LE_MDNS_CLIENT_H
#define LE_MDNS_CLIENT_H
#include "mdns_lease.h"

/* Default control socket of the shared libreecho-mdnsd supervisor. Daemons
 * pass their own path when the deployment moves it; the constant only keeps
 * the common case from being copy-pasted into every caller. */
#ifndef LE_MDNS_SOCKET
#define LE_MDNS_SOCKET "/run/libreecho/mdns.sock"
#endif

/* Nonblocking ESPHome registration AFTER the listener is ready and HA bit 1
 * is selected. Caller owns returned fd and closes it on disable/listener loss.
 * Metadata identity must match DeviceInfo, using stable configured/factory MAC.
 * Link mdns_client.c AND mdns_lease.c (the shared bounded wire encoder). */
int le_mdns_register_esphome(const char *path, unsigned int port,
                             const struct le_mdns_esphome_metadata *metadata);
/* Transitional obsolete Wyoming entry points: always -1, never advertise.
 * Kept only until old callers are removed by the parent integration. */
int le_mdns_register_wyoming(const char *path, unsigned int port);
int le_mdns_connect(const char *path, unsigned int port);
/* -1 lost/rejected lease, 0 no message yet, 1 locally accepted (not published). */
int le_mdns_receive(int fd);
/* Bounded readiness probe for the shared supervisor: 1 only when it reports a
 * running responder. Never blocks longer than the probe timeout. */
int le_mdns_status(const char *path);
#endif
