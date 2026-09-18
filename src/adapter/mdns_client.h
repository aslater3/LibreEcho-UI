#ifndef LE_MDNS_CLIENT_H
#define LE_MDNS_CLIENT_H

/* Default control socket of the shared libreecho-mdnsd supervisor. Daemons
 * pass their own path when the deployment moves it; the constant only keeps
 * the common case from being copy-pasted into every caller. */
#ifndef LE_MDNS_SOCKET
#define LE_MDNS_SOCKET "/run/libreecho/mdns.sock"
#endif

/* Nonblocking registration. Caller owns returned fd and closes to withdraw. */
int le_mdns_connect(const char *path, unsigned int port);
/* -1 lost/rejected lease, 0 no message yet, 1 locally accepted (not published). */
int le_mdns_receive(int fd);
/* Bounded readiness probe for the shared supervisor: 1 only when it reports a
 * running responder. Never blocks longer than the probe timeout. */
int le_mdns_status(const char *path);
#endif
