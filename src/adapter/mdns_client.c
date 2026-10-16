#define _GNU_SOURCE
#include "mdns_client.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LE_MDNS_PROBE_TIMEOUT_MS 250
#define LE_MDNS_STATUS "STATUS/1\n"
#define LE_MDNS_RUNNING "running\n"

static int probe_writable(int fd)
{
    struct pollfd descriptor;
    descriptor.fd = fd;
    descriptor.events = POLLOUT;
    descriptor.revents = 0;
    return poll(&descriptor, 1, LE_MDNS_PROBE_TIMEOUT_MS) > 0 &&
           (descriptor.revents & POLLOUT) != 0;
}

static int probe_readable(int fd)
{
    struct pollfd descriptor;
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    return poll(&descriptor, 1, LE_MDNS_PROBE_TIMEOUT_MS) > 0;
}

int le_mdns_register_esphome(const char *path, unsigned int port,
                             const struct le_mdns_esphome_metadata *metadata)
{
    struct sockaddr_un address;
    char message[LE_MDNS_ESPHOME_PACKET_MAX];
    int fd, length;
    if (!path || !path[0] || strlen(path) >= sizeof(address.sun_path))
        return -1;
    length = le_mdns_esphome_encode(message, sizeof(message), port, metadata);
    if (length < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd); return -1;
    }
    if (send(fd, message, (size_t)length, MSG_NOSIGNAL) != length) {
        close(fd); return -1;
    }
    return fd;
}
int le_mdns_register_wyoming(const char *path, unsigned int port)
{
    (void)path; (void)port; return -1;
}
int le_mdns_connect(const char *path, unsigned int port)
{
    return le_mdns_register_wyoming(path, port);
}
int le_mdns_receive(int fd)
{
    char response[32];
    ssize_t n = recv(fd, response, sizeof(response), MSG_DONTWAIT | MSG_TRUNC);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    if (n == 8 && !memcmp(response, "pending\n", 8)) return 1;
    return -1;
}
int le_mdns_status(const char *path)
{
    struct sockaddr_un address;
    char response[32];
    ssize_t n;
    int fd;
    if (!path || !path[0] || strlen(path) >= sizeof(address.sun_path))
        return 0;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0 &&
        errno != EINPROGRESS) {
        close(fd);
        return 0;
    }
    /* Every wait is bounded: a silent or hung supervisor must report
     * unavailable instead of stalling the caller's event loop. */
    if (!probe_writable(fd) ||
        send(fd, LE_MDNS_STATUS, sizeof(LE_MDNS_STATUS) - 1, MSG_NOSIGNAL) !=
            (ssize_t)(sizeof(LE_MDNS_STATUS) - 1) ||
        !probe_readable(fd)) {
        close(fd);
        return 0;
    }
    n = recv(fd, response, sizeof(response), MSG_DONTWAIT | MSG_TRUNC);
    close(fd);
    return n == (ssize_t)(sizeof(LE_MDNS_RUNNING) - 1) &&
           !memcmp(response, LE_MDNS_RUNNING, sizeof(LE_MDNS_RUNNING) - 1);
}
