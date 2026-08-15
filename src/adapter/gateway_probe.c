#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "gateway_probe.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SO_BINDTODEVICE
#define SO_BINDTODEVICE 25
#endif

#define IPV4_MIN_HEADER 20U
#define ICMP_HEADER_SIZE 8U
#define ICMP_ECHO_REPLY 0U
#define ICMP_ECHO_REQUEST 8U
#define MAX_PACKETS_PER_RECEIVE 16U

static uint16_t read_u16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void write_u16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)(value & 0xff);
}

uint16_t le_gateway_probe_checksum(const void *data, size_t length)
{
    const unsigned char *p = data;
    uint32_t sum = 0;

    while (length >= 2) {
        sum += read_u16(p);
        p += 2;
        length -= 2;
    }
    if (length)
        sum += (uint16_t)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

int le_gateway_probe_bindtodevice_failure_is_advisory(int err)
{
    return err == ENOPROTOOPT;
}

int le_gateway_probe_reply_matches(const void *packet, size_t length,
                                   uint32_t gateway_addr,
                                   uint16_t identifier, uint16_t sequence)
{
    const unsigned char *bytes = packet;
    const unsigned char *icmp;
    size_t ip_header_length;
    size_t ip_total_length;
    uint16_t fragment;

    if (!packet || length < IPV4_MIN_HEADER + ICMP_HEADER_SIZE ||
        (bytes[0] >> 4) != 4)
        return 0;
    ip_header_length = (size_t)(bytes[0] & 0x0fU) * 4U;
    ip_total_length = read_u16(bytes + 2);
    fragment = read_u16(bytes + 6);
    if (ip_header_length < IPV4_MIN_HEADER ||
        length < ip_header_length + ICMP_HEADER_SIZE ||
        ip_total_length != length || bytes[9] != IPPROTO_ICMP ||
        (fragment & (uint16_t)~0x4000U) != 0 ||
        le_gateway_probe_checksum(bytes, ip_header_length) != 0 ||
        memcmp(bytes + 12, &gateway_addr, sizeof(gateway_addr)))
        return 0;
    icmp = bytes + ip_header_length;
    if (icmp[0] != ICMP_ECHO_REPLY || icmp[1] != 0 ||
        read_u16(icmp + 4) != identifier ||
        read_u16(icmp + 6) != sequence)
        return 0;
    return le_gateway_probe_checksum(icmp, length - ip_header_length) == 0;
}

void le_gateway_probe_init(struct le_gateway_probe *probe)
{
    if (!probe)
        return;
    memset(probe, 0, sizeof(*probe));
    probe->fd = -1;
#ifdef LE_NETWORKD_TESTING
    probe->test_peer_fd = -1;
#endif
}

void le_gateway_probe_close(struct le_gateway_probe *probe)
{
    if (!probe)
        return;
    if (probe->fd >= 0)
        close(probe->fd);
#ifdef LE_NETWORKD_TESTING
    if (probe->test_peer_fd >= 0)
        close(probe->test_peer_fd);
#endif
    le_gateway_probe_init(probe);
}

#ifdef LE_NETWORKD_TESTING
static unsigned int test_probe_count;

static void test_probe_barrier(void)
{
    const char *path = getenv("LIBREECHO_GATEWAY_PROBE_BARRIER");
    const char *at_text = getenv("LIBREECHO_GATEWAY_PROBE_BARRIER_AT");
    char release[512];
    int marker;
    int wait_ms;

    if (!path || !path[0] || !at_text ||
        test_probe_count != (unsigned int)strtoul(at_text, NULL, 10))
        return;
    marker = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (marker >= 0)
        close(marker);
    if (snprintf(release, sizeof(release), "%s.release", path) >=
        (int)sizeof(release))
        return;
    for (wait_ms = 0; wait_ms < 2000 && access(release, F_OK) < 0;
         ++wait_ms)
        (void)poll(NULL, 0, 1);
}

static int test_next_result(void)
{
    static const char *script;
    static size_t offset;
    char value;

    if (!script)
        script = getenv("LIBREECHO_GATEWAY_PROBE_SCRIPT");
    if (!script)
        return -1;
    while (script[offset] == ',' || script[offset] == ' ' ||
           script[offset] == '\t' || script[offset] == '\n')
        ++offset;
    value = script[offset];
    if (value)
        ++offset;
    if (value == '1')
        return 1;
    if (value == '0')
        return 0;
    return -1;
}

static int test_probe_start(struct le_gateway_probe *probe, long long now_ms)
{
    int pair[2];
    int result;
    int flags;

    ++test_probe_count;
    result = test_next_result();

    if (result < 0) {
        errno = EOPNOTSUPP;
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) < 0)
        return -1;
    flags = fcntl(pair[0], F_GETFL, 0);
    if (flags < 0 || fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        int saved_errno = errno;
        close(pair[0]);
        close(pair[1]);
        errno = saved_errno;
        return -1;
    }
    (void)fcntl(pair[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(pair[1], F_SETFD, FD_CLOEXEC);
    probe->fd = pair[0];
    probe->test_peer_fd = pair[1];
    probe->deadline_ms = now_ms + LE_GATEWAY_PROBE_TIMEOUT_MS;
    probe->active = 1;
    if (result > 0 && send(pair[1], "1", 1, MSG_NOSIGNAL) != 1) {
        int saved_errno = errno;
        le_gateway_probe_close(probe);
        errno = saved_errno;
        return -1;
    }
    test_probe_barrier();
    return 0;
}
#endif

int le_gateway_probe_start(struct le_gateway_probe *probe,
                           const char *interface, const char *gateway,
                           long long now_ms)
{
    static uint16_t next_sequence;
    struct sockaddr_in target;
    unsigned char echo[ICMP_HEADER_SIZE];
    int fd;
    int flags;
    ssize_t sent;

    if (!probe || !interface || !*interface || !gateway || !*gateway) {
        errno = EINVAL;
        return -1;
    }
    le_gateway_probe_close(probe);
#ifdef LE_NETWORKD_TESTING
    return test_probe_start(probe, now_ms);
#endif
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    if (inet_pton(AF_INET, gateway, &target.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }
    fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0)
        return -1;
    flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface,
                   (socklen_t)(strlen(interface) + 1)) < 0 &&
        !le_gateway_probe_bindtodevice_failure_is_advisory(errno)) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    memset(echo, 0, sizeof(echo));
    probe->identifier = (uint16_t)((unsigned long)getpid() & 0xffffU);
    probe->sequence = ++next_sequence;
    echo[0] = ICMP_ECHO_REQUEST;
    write_u16(echo + 4, probe->identifier);
    write_u16(echo + 6, probe->sequence);
    write_u16(echo + 2, le_gateway_probe_checksum(echo, sizeof(echo)));
    sent = sendto(fd, echo, sizeof(echo), MSG_NOSIGNAL,
                  (struct sockaddr *)&target, sizeof(target));
    if (sent != (ssize_t)sizeof(echo)) {
        int saved_errno = sent < 0 ? errno : EIO;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    probe->fd = fd;
    probe->gateway_addr = target.sin_addr.s_addr;
    probe->deadline_ms = now_ms + LE_GATEWAY_PROBE_TIMEOUT_MS;
    probe->active = 1;
    return 0;
}

int le_gateway_probe_receive(struct le_gateway_probe *probe)
{
    unsigned char packet[2048];
    unsigned int packet_count;

    if (!probe || !probe->active || probe->fd < 0) {
        errno = EINVAL;
        return -1;
    }
#ifdef LE_NETWORKD_TESTING
    {
        char result;
        ssize_t received = recv(probe->fd, &result, 1, MSG_DONTWAIT);
        if (received == 1)
            return result == '1' ? 1 : 0;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        return received < 0 ? -1 : 0;
    }
#endif
    for (packet_count = 0; packet_count < MAX_PACKETS_PER_RECEIVE;
         ++packet_count) {
        ssize_t received = recv(probe->fd, packet, sizeof(packet), MSG_DONTWAIT);
        if (received > 0) {
            if (le_gateway_probe_reply_matches(packet, (size_t)received,
                                               probe->gateway_addr,
                                               probe->identifier,
                                               probe->sequence))
                return 1;
            continue;
        }
        if (received == 0)
            return 0;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    return 0;
}

int le_gateway_probe_timed_out(const struct le_gateway_probe *probe,
                               long long now_ms)
{
    return probe && probe->active && now_ms >= probe->deadline_ms;
}
