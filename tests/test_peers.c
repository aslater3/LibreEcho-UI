/* Device list (#413): the bounded DNS-SD browse for _libreecho._tcp.
 * Parser cases use hand-built response packets; the browse case runs a
 * forked legacy-unicast responder on loopback, so no multicast or Avahi is
 * needed on the build host. */
#define _DEFAULT_SOURCE
#include "peers.h"
#include <arpa/inet.h>
#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static const unsigned char service[] = "\012_libreecho\004_tcp\005local";

struct packet { unsigned char b[LE_PEERS_PACKET_MAX]; size_t n; };

static void put16(struct packet *p, unsigned v) { p->b[p->n++] = (unsigned char)(v >> 8); p->b[p->n++] = (unsigned char)v; }
static void put32(struct packet *p, unsigned long v) { put16(p, (unsigned)(v >> 16)); put16(p, (unsigned)(v & 0xffff)); }
static void raw(struct packet *p, const void *d, size_t n) { memcpy(p->b + p->n, d, n); p->n += n; }
static void header(struct packet *p, unsigned id, unsigned qd, unsigned an, unsigned ar)
{
    p->n = 0; put16(p, id); put16(p, 0x8400); put16(p, qd); put16(p, an); put16(p, 0); put16(p, ar);
}
/* <label>._libreecho._tcp.local as wire */
static size_t instance_wire(unsigned char *out, const char *label)
{
    size_t n = strlen(label);
    out[0] = (unsigned char)n; memcpy(out + 1, label, n); memcpy(out + 1 + n, service, sizeof(service));
    return 1 + n + sizeof(service);
}
static size_t host_wire(unsigned char *out, const char *host)
{
    size_t n = strlen(host);
    out[0] = (unsigned char)n; memcpy(out + 1, host, n); memcpy(out + 1 + n, "\005local", 7);
    return 1 + n + 7;
}
static void rr(struct packet *p, const unsigned char *name, size_t name_len, unsigned type, unsigned long ttl,
               const void *data, size_t data_len)
{
    raw(p, name, name_len); put16(p, type); put16(p, 0x8001); put32(p, ttl); put16(p, (unsigned)data_len);
    raw(p, data, data_len);
}
static void srv(struct packet *p, const char *label, unsigned port, const char *host)
{
    unsigned char name[256], data[300]; size_t n = instance_wire(name, label), d;
    data[0] = data[1] = data[2] = data[3] = 0; data[4] = (unsigned char)(port >> 8); data[5] = (unsigned char)port;
    d = 6 + host_wire(data + 6, host);
    rr(p, name, n, 33, 120, data, d);
}
static void txt(struct packet *p, const char *label, const char *const *items)
{
    unsigned char name[256], data[300]; size_t n = instance_wire(name, label), d = 0;
    for (; *items; ++items) { size_t k = strlen(*items); data[d++] = (unsigned char)k; memcpy(data + d, *items, k); d += k; }
    rr(p, name, n, 16, 4500, data, d);
}
static void ptr(struct packet *p, const char *label, unsigned long ttl)
{
    unsigned char data[256]; size_t d = instance_wire(data, label);
    rr(p, service, sizeof(service), 12, ttl, data, d);
}
static void a_record(struct packet *p, const char *host, const char *address)
{
    unsigned char name[256]; struct in_addr in; size_t n = host_wire(name, host);
    assert(inet_pton(AF_INET, address, &in) == 1);
    rr(p, name, n, 1, 120, &in, 4);
}

static void parser_cases(void)
{
    struct le_peers_scan scan;
    struct le_peer out[LE_PEERS_MAX];
    struct packet p;
    unsigned char query[512];
    const char *full[] = { "version=0.14.6", "room=Kitchen café", "path=/", NULL };
    const char *bad[] = { "version=0.14.6\"><script>", "room=a\001b", NULL };
    int truncated, n, i;
    uint32_t source = htonl(0x0a000002UL);

    /* The browse query: one PTR question, no answers (legacy unicast). */
    n = le_peers_browse_query(query, sizeof(query), 0x1234);
    assert(n == 12 + (int)sizeof(service) + 4);
    assert(query[0] == 0x12 && query[1] == 0x34 && query[5] == 1 && query[7] == 0 && query[11] == 0);
    assert(!memcmp(query + 12, service, sizeof(service)));
    assert(query[12 + sizeof(service) + 1] == 12);
    assert(le_peers_browse_query(query, 20, 1) < 0);

    /* A complete answer: PTR + SRV + TXT + A in one packet. */
    le_peers_scan_init(&scan, 7);
    header(&p, 7, 0, 3, 1); ptr(&p, "kitchen", 4500); srv(&p, "kitchen", 8080, "kitchen");
    txt(&p, "kitchen", full); a_record(&p, "kitchen", "10.0.0.9");
    assert(le_peers_scan_feed(&scan, p.b, p.n, source) == 0);
    assert(le_peers_scan_next_query(&scan, query, sizeof(query)) == 0);
    assert(le_peers_scan_result(&scan, out, LE_PEERS_MAX, &truncated) == 1 && !truncated);
    assert(!strcmp(out[0].name, "kitchen") && out[0].port == 8080);
    assert(!strcmp(out[0].version, "0.14.6") && !strcmp(out[0].room, "Kitchen café"));
    assert(!strcmp(out[0].address, "10.0.0.9"));

    /* Wrong id, a query (QR clear) or an error rcode are not ours. */
    p.b[1] = 8; assert(le_peers_scan_feed(&scan, p.b, p.n, source) < 0); p.b[1] = 7;
    p.b[2] = 0; assert(le_peers_scan_feed(&scan, p.b, p.n, source) < 0); p.b[2] = 0x84;
    p.b[3] = 3; assert(le_peers_scan_feed(&scan, p.b, p.n, source) < 0); p.b[3] = 0;

    /* PTR alone: SRV/TXT asked for once with an ANY query; no SRV means not listed;
     * address falls back to the responder when no A record arrived. */
    le_peers_scan_init(&scan, 9);
    header(&p, 9, 0, 1, 0); ptr(&p, "Bedroom", 4500);
    assert(le_peers_scan_feed(&scan, p.b, p.n, source) == 0);
    assert(le_peers_scan_result(&scan, out, LE_PEERS_MAX, &truncated) == 0);
    n = le_peers_scan_next_query(&scan, query, sizeof(query));
    assert(n > 0 && query[n - 3] == 255 && query[12] == 7 && !memcmp(query + 13, "Bedroom", 7));
    assert(le_peers_scan_next_query(&scan, query, sizeof(query)) == 0);
    header(&p, 9, 0, 2, 0); srv(&p, "BEDROOM", 80, "bedroom"); txt(&p, "bedroom", bad);
    assert(le_peers_scan_feed(&scan, p.b, p.n, source) == 0);
    assert(le_peers_scan_result(&scan, out, LE_PEERS_MAX, &truncated) == 1);
    /* Names compare case-insensitively; unsafe TXT values are dropped, not passed on. */
    assert(!strcmp(out[0].name, "Bedroom") && out[0].port == 80 && !strcmp(out[0].address, "10.0.0.2"));
    assert(!out[0].version[0] && !out[0].room[0]);

    /* Goodbye (TTL 0) records and other services are ignored. */
    le_peers_scan_init(&scan, 3);
    header(&p, 3, 0, 1, 0); ptr(&p, "gone", 0);
    assert(le_peers_scan_feed(&scan, p.b, p.n, source) == 0 && scan.count == 0);

    /* Compression pointers are followed; a forward or self pointer is malformed. */
    le_peers_scan_init(&scan, 5);
    header(&p, 5, 0, 1, 0);
    { size_t at = p.n; unsigned char data[16];
      raw(&p, service, sizeof(service)); put16(&p, 12); put16(&p, 1); put32(&p, 120);
      data[0] = 5; memcpy(data + 1, "attic", 5);
      data[6] = (unsigned char)(0xc0 | (at >> 8)); data[7] = (unsigned char)at;
      put16(&p, 8); raw(&p, data, 8); }
    assert(le_peers_scan_feed(&scan, p.b, p.n, source) == 0 && scan.count == 1);
    assert(!strcmp(scan.instances[0].label, "attic"));
    le_peers_scan_init(&scan, 5);
    header(&p, 5, 0, 1, 0); put16(&p, 0xc00c); put16(&p, 12); put16(&p, 1); put32(&p, 1); put16(&p, 0);
    assert(le_peers_scan_feed(&scan, p.b, p.n, source) < 0);

    /* Truncated records and absurd counts are rejected as a whole. */
    le_peers_scan_init(&scan, 7);
    header(&p, 7, 0, 3, 1); ptr(&p, "kitchen", 4500); srv(&p, "kitchen", 8080, "kitchen");
    for (i = 1; i < 40; ++i) assert(le_peers_scan_feed(&scan, p.b, p.n - (size_t)i, source) < 0);
    p.b[6] = 0xff; assert(le_peers_scan_feed(&scan, p.b, p.n, source) < 0);

    /* Never more than LE_PEERS_MAX devices; the overflow is reported. */
    le_peers_scan_init(&scan, 11);
    for (i = 0; i < LE_PEERS_MAX + 4; ++i) {
        char label[16]; snprintf(label, sizeof(label), "echo-%d", i);
        header(&p, 11, 0, 2, 0); ptr(&p, label, 120); srv(&p, label, 8080, label);
        assert(le_peers_scan_feed(&scan, p.b, p.n, source) == 0);
    }
    assert(le_peers_scan_result(&scan, out, LE_PEERS_MAX, &truncated) == LE_PEERS_MAX && truncated);
    assert(le_peers_scan_result(&scan, out, 3, &truncated) == 3 && truncated);
    puts("peers parser: query, answers, follow-up, case, TXT sanitising, goodbye, compression, bounds: ok");
}

/* Legacy-unicast responder: answers a PTR question with PTR only, and the
 * ANY follow-up with SRV + TXT + A, as Avahi may split them. */
static void responder(int fd)
{
    const char *items[] = { "version=0.14.6", "room=Hall", "path=/", NULL };
    for (;;) {
        unsigned char q[LE_PEERS_PACKET_MAX];
        struct sockaddr_in from; socklen_t len = sizeof(from);
        struct packet p;
        ssize_t n = recvfrom(fd, q, sizeof(q), 0, (struct sockaddr *)&from, &len);
        unsigned id;
        if (n < 12 + 4) continue;
        id = (unsigned)(q[0] << 8 | q[1]);
        if (q[n - 3] == 12) { header(&p, id, 0, 1, 0); ptr(&p, "hall-echo", 120); }
        else { header(&p, id, 0, 2, 1); srv(&p, "hall-echo", 8080, "hall-echo"); txt(&p, "hall-echo", items);
               a_record(&p, "hall-echo", "127.0.0.1"); }
        (void)sendto(fd, p.b, p.n, 0, (struct sockaddr *)&from, len);
    }
}

static void browse_case(void)
{
    struct sockaddr_in address; socklen_t len = sizeof(address);
    struct le_peer out[LE_PEERS_MAX];
    int fd = socket(AF_INET, SOCK_DGRAM, 0), truncated = 1, count;
    pid_t child;
    assert(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(getsockname(fd, (struct sockaddr *)&address, &len) == 0);
    child = fork();
    assert(child >= 0);
    if (!child) { alarm(10); responder(fd); _exit(0); }
    close(fd);
    count = le_peers_browse(&address, 600, out, LE_PEERS_MAX, &truncated);
    kill(child, SIGKILL); waitpid(child, NULL, 0);
    assert(count == 1 && !truncated);
    assert(!strcmp(out[0].name, "hall-echo") && out[0].port == 8080 && !strcmp(out[0].address, "127.0.0.1"));
    assert(!strcmp(out[0].version, "0.14.6") && !strcmp(out[0].room, "Hall"));

    /* Nobody answering is an empty list within the time bound, not an error. */
    address.sin_port = htons(9); /* discard: nothing replies */
    assert(le_peers_browse(&address, 200, out, LE_PEERS_MAX, &truncated) == 0 && !truncated);
    puts("peers browse: loopback legacy-unicast responder, follow-up query, silent network: ok");
}

int main(void)
{
    parser_cases();
    browse_case();
    return 0;
}
