#define _POSIX_C_SOURCE 200809L
#include "playback_status_client.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int le_playback_status_bus_drained(const char *path, const char *bus)
{
    char status[1536];
    char key[96];
    int fd;
    ssize_t count;

    if (!path || !bus ||
        (strcmp(bus, "system") && strcmp(bus, "announcement") &&
         strcmp(bus, "alarm")))
        return -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    count = read(fd, status, sizeof(status) - 1U);
    close(fd);
    if (count <= 0)
        return -1;
    status[count] = '\0';
    if (strstr(status, "\"drain\":")) {
        if (snprintf(key, sizeof(key), "\"%s\":{\"drained\":true", bus) >=
            (int)sizeof(key))
            return -1;
        if (strstr(status, key))
            return 1;
        if (snprintf(key, sizeof(key), "\"%s\":{\"drained\":false", bus) >=
            (int)sizeof(key))
            return -1;
        return strstr(status, key) ? 0 : -1;
    }
    if (snprintf(key, sizeof(key), "\"%s\":false", bus) >= (int)sizeof(key))
        return -1;
    if (strstr(status, key))
        return 1;
    if (snprintf(key, sizeof(key), "\"%s\":true", bus) >= (int)sizeof(key))
        return -1;
    return strstr(status, key) ? 0 : -1;
}
