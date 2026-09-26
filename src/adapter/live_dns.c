#define _POSIX_C_SOURCE 200809L

#include "live_dns.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DNS_PORT 53
#define DNS_MAX_PACKET 512
#define DNS_MAX_LABEL 63

static uint16_t get_u16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void put_u16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value >> 8);
    p[1] = (unsigned char)value;
}

static int read_nameserver(const char *path, struct sockaddr_in *server)
{
    char buffer[1024];
    char *line;
    FILE *file;

    memset(server, 0, sizeof(*server));
    server->sin_family = AF_INET;
    server->sin_port = htons(DNS_PORT);
    file = fopen(path ? path : "/etc/resolv.conf", "r");
    if (!file)
        return -1;
    while (fgets(buffer, sizeof(buffer), file)) {
        char address[64];
        char *value;
        char *end;
        char *port;

        line = buffer;
        while (*line == ' ' || *line == '\t')
            ++line;
        if (strncmp(line, "nameserver", 10) ||
            (line[10] != ' ' && line[10] != '\t'))
            continue;
        value = line + 10;
        while (*value == ' ' || *value == '\t')
            ++value;
        end = value;
        while (*end && *end != ' ' && *end != '\t' && *end != '\r' &&
               *end != '\n' && (size_t)(end - value) + 1U < sizeof(address))
            ++end;
        memcpy(address, value, (size_t)(end - value));
        address[end - value] = '\0';
        /* Optional test-only port on an IPv4 nameserver. */
        port = strrchr(address, ':');
        if (port) {
            long parsed;

            *port++ = '\0';
            parsed = strtol(port, NULL, 10);
            if (parsed <= 0 || parsed > 65535)
                continue;
            server->sin_port = htons((uint16_t)parsed);
        }
        if (inet_pton(AF_INET, address, &server->sin_addr) == 1) {
            fclose(file);
            return 0;
        }
    }
    fclose(file);
    return -1;
}

static int random_id(uint16_t *id)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    unsigned char bytes[2];
    ssize_t got;

    if (fd < 0)
        return -1;
    do {
        got = read(fd, bytes, sizeof(bytes));
    } while (got < 0 && errno == EINTR);
    close(fd);
    if (got != (ssize_t)sizeof(bytes))
        return -1;
    *id = (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
    return 0;
}

static int encode_name(unsigned char *packet, size_t size, size_t *used,
                       const char *hostname)
{
    const char *part = hostname;

    if (!hostname || !hostname[0] || strlen(hostname) > 253U)
        return -1;
    while (*part) {
        const char *dot = strchr(part, '.');
        size_t length = dot ? (size_t)(dot - part) : strlen(part);

        if (!length || length > DNS_MAX_LABEL || *used + 1U + length >= size)
            return -1;
        packet[(*used)++] = (unsigned char)length;
        memcpy(packet + *used, part, length);
        *used += length;
        if (!dot)
            break;
        part = dot + 1;
    }
    if (*used >= size)
        return -1;
    packet[(*used)++] = 0;
    return 0;
}

/* Skip a DNS name at `offset`, following no pointers but accepting them. */
static int skip_name(const unsigned char *packet, size_t size, size_t *offset)
{
    size_t position = *offset;
    unsigned int labels = 0;

    while (position < size && labels++ < 128U) {
        unsigned char length = packet[position++];

        if (!length) {
            *offset = position;
            return 0;
        }
        if ((length & 0xc0U) == 0xc0U) {
            if (position >= size)
                return -1;
            *offset = position + 1U;
            return 0;
        }
        if (length > DNS_MAX_LABEL || position + length > size)
            return -1;
        position += length;
    }
    return -1;
}

int le_live_dns_resolve_ipv4(const char *hostname, const char *resolv_conf,
                             struct in_addr *address, int timeout_ms)
{
    unsigned char packet[DNS_MAX_PACKET];
    struct sockaddr_in server;
    struct pollfd descriptor;
    uint16_t id;
    size_t used = 12;
    size_t offset;
    ssize_t count;
    int fd;
    unsigned int i;
    uint16_t answers;

    if (!hostname || !address || read_nameserver(resolv_conf, &server) < 0 ||
        random_id(&id) < 0)
        return -1;
    memset(packet, 0, sizeof(packet));
    put_u16(packet, id);
    put_u16(packet + 2, 0x0100);          /* recursion desired */
    put_u16(packet + 4, 1);               /* one question */
    if (encode_name(packet, sizeof(packet), &used, hostname) < 0 ||
        used + 4U > sizeof(packet))
        return -1;
    put_u16(packet + used, 1);            /* A */
    put_u16(packet + used + 2U, 1);       /* IN */
    used += 4U;

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0)
        return -1;
    count = sendto(fd, packet, used, 0, (struct sockaddr *)&server,
                   sizeof(server));
    if (count != (ssize_t)used) {
        close(fd);
        return -1;
    }
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    if (poll(&descriptor, 1, timeout_ms > 0 ? timeout_ms : 3000) <= 0) {
        close(fd);
        return -1;
    }
    count = recv(fd, packet, sizeof(packet), 0);
    close(fd);
    if (count < 12 || get_u16(packet) != id || !(packet[2] & 0x80U) ||
        (packet[3] & 0x0fU) != 0 || get_u16(packet + 4) != 1)
        return -1;
    answers = get_u16(packet + 6);
    offset = 12;
    if (skip_name(packet, (size_t)count, &offset) < 0 || offset + 4U > (size_t)count)
        return -1;
    offset += 4U;                           /* question type/class */
    for (i = 0; i < answers; ++i) {
        uint16_t type;
        uint16_t klass;
        uint16_t length;

        if (skip_name(packet, (size_t)count, &offset) < 0 ||
            offset + 10U > (size_t)count)
            return -1;
        type = get_u16(packet + offset);
        klass = get_u16(packet + offset + 2U);
        length = get_u16(packet + offset + 8U);
        offset += 10U;
        if (offset + length > (size_t)count)
            return -1;
        if (type == 1 && klass == 1 && length == 4) {
            memcpy(address, packet + offset, 4);
            return 0;
        }
        offset += length;
    }
    return -1;
}