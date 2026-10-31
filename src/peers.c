#define _DEFAULT_SOURCE
#include "peers.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DNS_PTR 12
#define DNS_TXT 16
#define DNS_SRV 33
#define DNS_A 1
#define DNS_ANY 255
#define DNS_IN 1
#define MAX_RECORDS 96
#define MAX_JUMPS 32

static const unsigned char service_wire[] = "\012_libreecho\004_tcp\005local";
#define SERVICE_WIRE_LEN (sizeof(service_wire)) /* includes the root label */

static long monotonic_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return 0;
    return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static uint16_t get16(const unsigned char *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void put16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }
static int wire_equal(const unsigned char *a, size_t an, const unsigned char *b, size_t bn)
{
    size_t i;
    if (an != bn) return 0;
    for (i = 0; i < an; ++i) if (lower(a[i]) != lower(b[i])) return 0;
    return 1;
}
/* UTF-8 without control characters, as the advertising side requires. */
static int text_valid(const unsigned char *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        unsigned int cp, minimum, count, j;
        unsigned char c = s[i++];
        if (c < 0x80) { if (c < 0x20 || c == 0x7f) return 0; continue; }
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; count = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; count = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; count = 3; minimum = 0x10000; }
        else return 0;
        if (count > n - i) return 0;
        for (j = 0; j < count; ++j) {
            c = s[i++];
            if ((c & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (c & 63);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || (cp >= 0x80 && cp <= 0x9f))
            return 0;
    }
    return 1;
}
static int version_valid(const unsigned char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        unsigned char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              c == '.' || c == '-' || c == '_' || c == '+')) return 0;
    }
    return 1;
}
/* Reads a possibly compressed name at *offset into uncompressed wire form
 * (with the terminating root label). Advances *offset past the name in the
 * original position. Every pointer must point backwards and the walk is
 * capped, so a looping packet cannot spin. */
static int read_name(const unsigned char *packet, size_t length, size_t *offset,
                     unsigned char *out, size_t *out_len)
{
    size_t p = *offset, used = 0, end = 0;
    int jumps = 0, jumped = 0;
    for (;;) {
        unsigned int n;
        if (p >= length) return -1;
        n = packet[p];
        if ((n & 0xc0) == 0xc0) {
            size_t target;
            if (p + 1 >= length || ++jumps > MAX_JUMPS) return -1;
            target = ((size_t)(n & 0x3f) << 8) | packet[p + 1];
            if (target >= p) return -1;
            if (!jumped) { end = p + 2; jumped = 1; }
            p = target;
            continue;
        }
        if (n & 0xc0) return -1;
        if (used + 1 + n > LE_PEERS_NAME_MAX) return -1;
        if (!n) { out[used++] = 0; break; }
        if (p + 1 + n > length) return -1;
        memcpy(out + used, packet + p, 1 + (size_t)n);
        used += 1 + (size_t)n;
        p += 1 + (size_t)n;
    }
    *offset = jumped ? end : p + 1;
    *out_len = used;
    return 0;
}
/* An instance name is <label>._libreecho._tcp.local. */
static int instance_label(const unsigned char *wire, size_t wire_len, char *label, size_t capacity)
{
    size_t n;
    if (wire_len < 2) return -1;
    n = wire[0];
    if (!n || n >= capacity || 1 + n + SERVICE_WIRE_LEN != wire_len ||
        !wire_equal(wire + 1 + n, SERVICE_WIRE_LEN, service_wire, SERVICE_WIRE_LEN) ||
        !text_valid(wire + 1, n)) return -1;
    memcpy(label, wire + 1, n);
    label[n] = 0;
    return 0;
}
static struct le_peers_instance *instance_for(struct le_peers_scan *scan, const unsigned char *wire,
                                              size_t wire_len)
{
    struct le_peers_instance *instance;
    char label[64];
    size_t i;
    for (i = 0; i < scan->count; ++i)
        if (wire_equal(scan->instances[i].wire, scan->instances[i].wire_len, wire, wire_len))
            return &scan->instances[i];
    if (instance_label(wire, wire_len, label, sizeof(label)) < 0) return NULL;
    if (scan->count == LE_PEERS_MAX) { scan->overflow = 1; return NULL; }
    instance = &scan->instances[scan->count++];
    memset(instance, 0, sizeof(*instance));
    memcpy(instance->wire, wire, wire_len);
    instance->wire_len = wire_len;
    memcpy(instance->label, label, sizeof(label));
    return instance;
}
static void read_txt(struct le_peers_instance *instance, const unsigned char *data, size_t length)
{
    size_t p = 0;
    instance->version[0] = instance->room[0] = 0;
    while (p < length) {
        size_t n = data[p++];
        const unsigned char *item = data + p;
        if (n > length - p) break;
        p += n;
        if (n > 8 && !memcmp(item, "version=", 8) && n - 8 < sizeof(instance->version) &&
            version_valid(item + 8, n - 8)) {
            memcpy(instance->version, item + 8, n - 8); instance->version[n - 8] = 0;
        } else if (n >= 5 && !memcmp(item, "room=", 5) && n - 5 < sizeof(instance->room) &&
                   text_valid(item + 5, n - 5)) {
            memcpy(instance->room, item + 5, n - 5); instance->room[n - 5] = 0;
        }
    }
    instance->have_txt = 1;
}
static void remember_host(struct le_peers_scan *scan, const unsigned char *wire, size_t wire_len,
                          uint32_t address)
{
    size_t i;
    for (i = 0; i < scan->host_count; ++i)
        if (wire_equal(scan->hosts[i].wire, scan->hosts[i].wire_len, wire, wire_len)) {
            scan->hosts[i].address = address; return;
        }
    if (scan->host_count == LE_PEERS_HOSTS) return;
    memcpy(scan->hosts[scan->host_count].wire, wire, wire_len);
    scan->hosts[scan->host_count].wire_len = wire_len;
    scan->hosts[scan->host_count++].address = address;
}

void le_peers_scan_init(struct le_peers_scan *scan, uint16_t id)
{
    memset(scan, 0, sizeof(*scan));
    scan->id = id;
}
int le_peers_build_query(unsigned char *out, size_t capacity, uint16_t id,
                         const unsigned char *wire, size_t wire_len, uint16_t qtype)
{
    if (!out || !wire || !wire_len || wire_len > LE_PEERS_NAME_MAX || capacity < 12 + wire_len + 4)
        return -1;
    memset(out, 0, 12);
    put16(out, id);
    put16(out + 4, 1); /* one question; legacy unicast carries no answers */
    memcpy(out + 12, wire, wire_len);
    put16(out + 12 + wire_len, qtype);
    put16(out + 14 + wire_len, DNS_IN);
    return (int)(16 + wire_len);
}
int le_peers_browse_query(unsigned char *out, size_t capacity, uint16_t id)
{
    return le_peers_build_query(out, capacity, id, service_wire, SERVICE_WIRE_LEN, DNS_PTR);
}
int le_peers_scan_feed(struct le_peers_scan *scan, const unsigned char *packet, size_t length,
                       uint32_t source)
{
    unsigned char name[LE_PEERS_NAME_MAX], data_name[LE_PEERS_NAME_MAX];
    size_t p = 12, name_len, data_len, i;
    unsigned int questions, records;
    if (!scan || !packet || length < 12 || length > LE_PEERS_PACKET_MAX ||
        get16(packet) != scan->id || !(packet[2] & 0x80) || (packet[2] & 0x78) || (packet[3] & 0x0f))
        return -1;
    questions = get16(packet + 4);
    records = (unsigned int)get16(packet + 6) + get16(packet + 8) + get16(packet + 10);
    if (questions > 4 || records > MAX_RECORDS) return -1;
    for (i = 0; i < questions; ++i) {
        if (read_name(packet, length, &p, name, &name_len) < 0 || p + 4 > length) return -1;
        p += 4;
    }
    for (i = 0; i < records; ++i) {
        unsigned int type, klass, rdlength;
        unsigned long ttl;
        size_t data;
        struct le_peers_instance *instance;
        if (read_name(packet, length, &p, name, &name_len) < 0 || p + 10 > length) return -1;
        type = get16(packet + p);
        klass = get16(packet + p + 2) & 0x7fff;
        ttl = ((unsigned long)packet[p + 4] << 24) | ((unsigned long)packet[p + 5] << 16) |
              ((unsigned long)packet[p + 6] << 8) | packet[p + 7];
        rdlength = get16(packet + p + 8);
        data = p + 10;
        if (rdlength > length - data) return -1;
        p = data + rdlength;
        /* A zero TTL is a goodbye: that device is leaving. */
        if (klass != DNS_IN || !ttl) continue;
        if (type == DNS_PTR && wire_equal(name, name_len, service_wire, SERVICE_WIRE_LEN)) {
            size_t q = data;
            if (read_name(packet, data + rdlength, &q, data_name, &data_len) < 0) continue;
            instance = instance_for(scan, data_name, data_len);
            if (instance && !instance->source) instance->source = source;
        } else if (type == DNS_SRV && rdlength > 6) {
            size_t q = data + 6;
            unsigned int port = get16(packet + data + 4);
            if (!port || read_name(packet, data + rdlength, &q, data_name, &data_len) < 0) continue;
            instance = instance_for(scan, name, name_len);
            if (!instance) continue;
            instance->port = port;
            memcpy(instance->target, data_name, data_len);
            instance->target_len = data_len;
            instance->source = source;
            instance->have_srv = 1;
        } else if (type == DNS_TXT) {
            instance = instance_for(scan, name, name_len);
            if (instance) read_txt(instance, packet + data, rdlength);
        } else if (type == DNS_A && rdlength == 4) {
            uint32_t address;
            memcpy(&address, packet + data, 4);
            remember_host(scan, name, name_len, address);
        }
    }
    return 0;
}
int le_peers_scan_next_query(struct le_peers_scan *scan, unsigned char *out, size_t capacity)
{
    size_t i;
    for (i = 0; i < scan->count; ++i) {
        struct le_peers_instance *instance = &scan->instances[i];
        int n;
        if (instance->queried || (instance->have_srv && instance->have_txt)) continue;
        instance->queried = 1;
        n = le_peers_build_query(out, capacity, scan->id, instance->wire, instance->wire_len, DNS_ANY);
        if (n > 0) return n;
    }
    return 0;
}
size_t le_peers_scan_result(const struct le_peers_scan *scan, struct le_peer *out, size_t capacity,
                            int *truncated)
{
    size_t i, j, count = 0;
    if (truncated) *truncated = scan->overflow;
    for (i = 0; i < scan->count; ++i) {
        const struct le_peers_instance *instance = &scan->instances[i];
        uint32_t address = instance->source;
        struct in_addr in;
        if (!instance->have_srv) continue;
        if (count == capacity) { if (truncated) *truncated = 1; break; }
        for (j = 0; j < scan->host_count; ++j)
            if (wire_equal(scan->hosts[j].wire, scan->hosts[j].wire_len, instance->target,
                           instance->target_len)) { address = scan->hosts[j].address; break; }
        if (!address) continue;
        memset(&out[count], 0, sizeof(out[count]));
        memcpy(out[count].name, instance->label, sizeof(out[count].name));
        memcpy(out[count].version, instance->version, sizeof(out[count].version));
        memcpy(out[count].room, instance->room, sizeof(out[count].room));
        in.s_addr = address;
        if (!inet_ntop(AF_INET, &in, out[count].address, sizeof(out[count].address))) continue;
        out[count].port = instance->port;
        ++count;
    }
    return count;
}
int le_peers_browse(const struct sockaddr_in *destination, int timeout_ms, struct le_peer *out,
                    size_t capacity, int *truncated)
{
    struct le_peers_scan scan;
    struct sockaddr_in target, local;
    unsigned char packet[LE_PEERS_PACKET_MAX];
    unsigned char ttl = 255;
    long start = monotonic_ms(), deadline, resend;
    int fd, query_len, resent = 0;
    if (truncated) *truncated = 0;
    if (timeout_ms < 0) timeout_ms = 0;
    if (destination) target = *destination;
    else {
        memset(&target, 0, sizeof(target));
        target.sin_family = AF_INET;
        target.sin_port = htons(5353);
        target.sin_addr.s_addr = htonl(0xe00000fbUL); /* 224.0.0.251 */
    }
    le_peers_scan_init(&scan, (uint16_t)(((unsigned)getpid() ^ (unsigned)start) | 1u));
    fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) { close(fd); return -1; }
    (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    query_len = le_peers_browse_query(packet, sizeof(packet), scan.id);
    if (query_len < 0 ||
        sendto(fd, packet, (size_t)query_len, 0, (struct sockaddr *)&target, sizeof(target)) != query_len) {
        close(fd); return -1;
    }
    deadline = start + timeout_ms;
    /* Wi-Fi drops multicast now and then; ask a second time part way through. */
    resend = start + timeout_ms / 3;
    for (;;) {
        struct pollfd descriptor;
        long now = monotonic_ms(), wait;
        int ready;
        if (now >= deadline) break;
        if (!resent && now >= resend) {
            resent = 1;
            query_len = le_peers_browse_query(packet, sizeof(packet), scan.id);
            if (query_len > 0)
                (void)sendto(fd, packet, (size_t)query_len, 0, (struct sockaddr *)&target, sizeof(target));
        }
        wait = (resent ? deadline : resend) - now;
        if (wait < 1) wait = 1;
        descriptor.fd = fd; descriptor.events = POLLIN; descriptor.revents = 0;
        ready = poll(&descriptor, 1, (int)wait);
        if (ready < 0 && errno != EINTR) break;
        if (ready <= 0) continue;
        for (;;) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            ssize_t n = recvfrom(fd, packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC,
                                 (struct sockaddr *)&from, &from_len);
            if (n < 0) break;
            if ((size_t)n > sizeof(packet) || from_len < sizeof(from) || from.sin_family != AF_INET)
                continue;
            (void)le_peers_scan_feed(&scan, packet, (size_t)n, from.sin_addr.s_addr);
        }
        while ((query_len = le_peers_scan_next_query(&scan, packet, sizeof(packet))) > 0)
            (void)sendto(fd, packet, (size_t)query_len, 0, (struct sockaddr *)&target, sizeof(target));
    }
    close(fd);
    return (int)le_peers_scan_result(&scan, out, capacity, truncated);
}
