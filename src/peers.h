#ifndef LE_PEERS_H
#define LE_PEERS_H
#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

/* Other LibreEcho devices on the LAN (#413), found by one bounded DNS-SD
 * browse for _libreecho._tcp.local. The query is an mDNS "legacy unicast"
 * query (RFC 6762 section 6.7) from an ephemeral port, so responders answer
 * straight to this socket and nothing has to share port 5353 with Avahi.
 * Every limit is fixed: at most LE_PEERS_MAX devices, one packet buffer, a
 * caller-supplied time bound. Nothing here allocates. */
#define LE_PEERS_MAX 16
#define LE_PEERS_HOSTS (LE_PEERS_MAX * 2)
#define LE_PEERS_PACKET_MAX 1500
#define LE_PEERS_NAME_MAX 256

struct le_peer {
    char name[64];     /* DNS-SD instance label (the device hostname) */
    char version[32];  /* TXT version, [0-9A-Za-z._+-] or empty */
    char room[64];     /* TXT room, UTF-8 without controls, may be empty */
    char address[16];  /* dotted IPv4 */
    unsigned int port;
};

struct le_peers_instance {
    unsigned char wire[LE_PEERS_NAME_MAX], target[LE_PEERS_NAME_MAX];
    size_t wire_len, target_len;
    char label[64], version[32], room[64];
    unsigned int port;
    uint32_t source;
    int have_srv, have_txt, queried;
};
struct le_peers_host {
    unsigned char wire[LE_PEERS_NAME_MAX];
    size_t wire_len;
    uint32_t address;
};
/* Collected state of one browse; plain data so the parser is testable
 * without sockets. */
struct le_peers_scan {
    struct le_peers_instance instances[LE_PEERS_MAX];
    struct le_peers_host hosts[LE_PEERS_HOSTS];
    size_t count, host_count;
    int overflow;
    uint16_t id;
};

void le_peers_scan_init(struct le_peers_scan *scan, uint16_t id);
/* A DNS query with one question (wire-format name). Returns its length or -1. */
int le_peers_build_query(unsigned char *out, size_t capacity, uint16_t id,
                         const unsigned char *wire, size_t wire_len, uint16_t qtype);
/* The PTR query that starts a browse. */
int le_peers_browse_query(unsigned char *out, size_t capacity, uint16_t id);
/* Feeds one response datagram that came from source (network-order IPv4).
 * Packets that are not a well-formed response to scan->id are ignored as a
 * whole. Returns 0 when the packet was used, -1 when it was ignored. */
int le_peers_scan_feed(struct le_peers_scan *scan, const unsigned char *packet, size_t length,
                       uint32_t source);
/* Builds the follow-up query (QTYPE ANY) for the next instance that still
 * lacks SRV or TXT and has not been asked yet. Returns its length, or 0 when
 * nothing is left to ask. */
int le_peers_scan_next_query(struct le_peers_scan *scan, unsigned char *out, size_t capacity);
/* Copies instances that have an SRV record into out; *truncated is set when
 * more instances were seen than fit in the scan or in out. Returns the count. */
size_t le_peers_scan_result(const struct le_peers_scan *scan, struct le_peer *out, size_t capacity,
                            int *truncated);

/* One browse. destination NULL means 224.0.0.251:5353. Waits at most
 * timeout_ms in total. Returns the device count, or -1 when no socket could
 * be used at all. */
int le_peers_browse(const struct sockaddr_in *destination, int timeout_ms, struct le_peer *out,
                    size_t capacity, int *truncated);
#endif
