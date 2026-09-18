#define _POSIX_C_SOURCE 200809L
#include "mdns_lease.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void le_mdns_lease_init(struct le_mdns_lease *lease, int directory_fd)
{
    memset(lease, 0, sizeof(*lease));
    lease->directory_fd = directory_fd;
    lease->owner_fd = -1;
}

int le_mdns_lease_withdraw(struct le_mdns_lease *lease, int owner_fd)
{
    if (owner_fd < 0 || lease->owner_fd != owner_fd)
        return -1;
    if (lease->filename[0] &&
        unlinkat(lease->directory_fd, lease->filename, 0) < 0 && errno != ENOENT)
        return -1;
    lease->filename[0] = 0;
    lease->owner_fd = -1;
    return 0;
}

int le_mdns_lease_register(struct le_mdns_lease *lease, int owner_fd,
                           unsigned int port)
{
    char xml[512], name[64], temp[64];
    int fd, length, result = -1;
    size_t written = 0;
    if (owner_fd < 0 || port < 1 || port > 65535 ||
        (lease->owner_fd >= 0 && lease->owner_fd != owner_fd) ||
        lease->generation == ULONG_MAX)
        return -1;
    ++lease->generation;
    (void)snprintf(name, sizeof(name), "wyoming-%lu.service", lease->generation);
    (void)snprintf(temp, sizeof(temp), "wyoming-%lu.tmp", lease->generation);
    length = snprintf(xml, sizeof(xml),
        "<?xml version=\"1.0\"?><!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
        "<service-group><name replace-wildcards=\"yes\">LibreEcho %%h</name>"
        "<service><type>_wyoming._tcp</type><port>%u</port></service>"
        "</service-group>\n", port);
    if (length < 0 || (size_t)length >= sizeof(xml))
        return -1;
    fd = openat(lease->directory_fd, temp,
                O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    while (written < (size_t)length) {
        ssize_t count = write(fd, xml + written, (size_t)length - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            goto out;
        written += (size_t)count;
    }
    if (fsync(fd) < 0)
        goto out;
    if (close(fd) < 0) { fd = -1; goto out; }
    fd = -1;
    /* Never replace an unexpected existing generation, including symlinks. */
    if (linkat(lease->directory_fd, temp, lease->directory_fd, name, 0) < 0)
        goto out;
    if (lease->filename[0] &&
        unlinkat(lease->directory_fd, lease->filename, 0) < 0 && errno != ENOENT) {
        (void)unlinkat(lease->directory_fd, name, 0);
        goto out;
    }
    memcpy(lease->filename, name, strlen(name) + 1);
    lease->owner_fd = owner_fd;
    result = 0;
out:
    if (fd >= 0) (void)close(fd);
    (void)unlinkat(lease->directory_fd, temp, 0);
    return result;
}
