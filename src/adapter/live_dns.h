#ifndef LIBREECHO_LIVE_DNS_H
#define LIBREECHO_LIVE_DNS_H

/*
 * Small DNS A-record resolver for the static ARM32 lived artifact.
 *
 * Static glibc getaddrinfo() needs glibc NSS modules at runtime. The target is
 * musl, so that link succeeds and lookup fails later on the device. This
 * resolver reads the active nameserver from resolv.conf and sends one bounded
 * UDP query directly, removing the libc/NSS runtime dependency.
 */

#include <netinet/in.h>

/*
 * Resolve `hostname` to one IPv4 address.
 * `resolv_conf` defaults to /etc/resolv.conf when NULL. For tests only, a
 * nameserver line may include `:port`; production resolv.conf uses port 53.
 * Returns 0 on success, -1 on timeout, malformed response, or no A answer.
 */
int le_live_dns_resolve_ipv4(const char *hostname, const char *resolv_conf,
                             struct in_addr *address, int timeout_ms);

#endif