#define _POSIX_C_SOURCE 200809L

#include "inherited_fds.h"

#include <dirent.h>
#include <stdlib.h>
#include <unistd.h>

void le_close_inherited_fds(int keep_fd)
{
    DIR *directory;
    struct dirent *entry;
    int scan_fd;

    /* The worker must not retain listeners or unrelated client sockets. */
    directory = opendir("/proc/self/fd");
    if (directory) {
        scan_fd = dirfd(directory);
        while ((entry = readdir(directory)) != NULL) {
            char *end;
            long inherited = strtol(entry->d_name, &end, 10);

            if (*entry->d_name && !*end && inherited >= 3 &&
                inherited != keep_fd && inherited != scan_fd)
                close((int)inherited);
        }
        closedir(directory);
        return;
    }

    /* Keep a bounded fallback for test hosts without procfs. */
    {
        long limit = sysconf(_SC_OPEN_MAX);
        int fd;

        if (limit < 3 || limit > 65536)
            limit = 65536;
        for (fd = 3; fd < limit; ++fd)
            if (fd != keep_fd)
                close(fd);
    }
}
