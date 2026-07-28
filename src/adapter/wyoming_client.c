#define _POSIX_C_SOURCE 200809L

#include "wyoming_client.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static int parse_uri(const char *uri, char *host, size_t host_size,
                     char *service, size_t service_size)
{
    const char *address;
    const char *port;
    size_t host_length;
    unsigned long port_number = 0;
    const char *p;

    if (!uri || strncmp(uri, "tcp://", 6) || strlen(uri) >= LE_WYOMING_URI_MAX)
        return -1;
    address = uri + 6;
    if (*address == '[') {
        const char *end = strchr(address + 1, ']');

        if (!end || end[1] != ':')
            return -1;
        host_length = (size_t)(end - address - 1);
        port = end + 2;
        ++address;
    } else {
        port = strrchr(address, ':');
        if (!port)
            return -1;
        host_length = (size_t)(port - address);
        ++port;
    }
    if (!host_length || host_length >= host_size || !*port ||
        strlen(port) >= service_size)
        return -1;
    for (p = port; *p; ++p) {
        if (*p < '0' || *p > '9')
            return -1;
        port_number = port_number * 10UL + (unsigned long)(*p - '0');
        if (port_number > 65535UL)
            return -1;
    }
    if (!port_number)
        return -1;
    memcpy(host, address, host_length);
    host[host_length] = '\0';
    snprintf(service, service_size, "%lu", port_number);
    return 0;
}

int le_wyoming_uri_valid(const char *uri)
{
    char host[256];
    char service[8];

    return parse_uri(uri, host, sizeof(host), service, sizeof(service)) == 0;
}

static int connect_address(const struct addrinfo *address, int timeout_ms)
{
    struct pollfd descriptor;
    int fd;
    int flags;
    int error = 0;
    socklen_t error_size = sizeof(error);
    struct timeval io_timeout;

    fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0)
        return -1;
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        goto fail;
    if (connect(fd, address->ai_addr, address->ai_addrlen) < 0 &&
        errno != EINPROGRESS)
        goto fail;
    descriptor.fd = fd;
    descriptor.events = POLLOUT;
    descriptor.revents = 0;
    if (poll(&descriptor, 1, timeout_ms) <= 0 ||
        !(descriptor.revents & POLLOUT) ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) < 0 ||
        error != 0)
        goto fail;
    if (fcntl(fd, F_SETFL, flags) < 0)
        goto fail;
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    io_timeout.tv_sec = 45;
    io_timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     &io_timeout, sizeof(io_timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                     &io_timeout, sizeof(io_timeout));
    return fd;

fail:
    close(fd);
    return -1;
}

int le_wyoming_connect(const char *uri, int timeout_ms)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char host[256];
    char service[8];
    int fd = -1;

    if (timeout_ms < 1 ||
        parse_uri(uri, host, sizeof(host), service, sizeof(service)) < 0)
        return -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, service, &hints, &addresses) != 0)
        return -1;
    for (address = addresses; address; address = address->ai_next) {
        fd = connect_address(address, timeout_ms);
        if (fd >= 0)
            break;
    }
    freeaddrinfo(addresses);
    return fd;
}
