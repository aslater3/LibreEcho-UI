#ifndef LIBREECHO_GATEWAY_PROBE_H
#define LIBREECHO_GATEWAY_PROBE_H

#include <stddef.h>
#include <stdint.h>

#ifdef LE_NETWORKD_TESTING
#define LE_GATEWAY_PROBE_TIMEOUT_MS 10LL
#else
#define LE_GATEWAY_PROBE_TIMEOUT_MS 2000LL
#endif

struct le_gateway_probe {
    int fd;
    uint16_t identifier;
    uint16_t sequence;
    uint32_t gateway_addr;
    long long deadline_ms;
    int active;
#ifdef LE_NETWORKD_TESTING
    int test_peer_fd;
#endif
};

uint16_t le_gateway_probe_checksum(const void *data, size_t length);
int le_gateway_probe_reply_matches(const void *packet, size_t length,
                                   uint32_t gateway_addr,
                                   uint16_t identifier, uint16_t sequence);
/* SO_BINDTODEVICE is a scoping optimisation, not a correctness requirement:
 * reply matching already validates gateway address, identifier, sequence and
 * both checksums.  Returns 1 when a failed setsockopt(SO_BINDTODEVICE) with
 * errno == err must not abort probing.  The MT8163 kernel reports ENOPROTOOPT
 * for this option on raw ICMP sockets, so liveness probing must survive it. */
int le_gateway_probe_bindtodevice_failure_is_advisory(int err);
void le_gateway_probe_init(struct le_gateway_probe *probe);
int le_gateway_probe_start(struct le_gateway_probe *probe,
                           const char *interface, const char *gateway,
                           long long now_ms);
int le_gateway_probe_receive(struct le_gateway_probe *probe);
int le_gateway_probe_timed_out(const struct le_gateway_probe *probe,
                               long long now_ms);
void le_gateway_probe_close(struct le_gateway_probe *probe);

#endif
