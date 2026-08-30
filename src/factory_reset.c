#define _GNU_SOURCE

#include "factory_reset.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int clear_contents(int fd)
{
    DIR *directory;
    struct dirent *entry;
    int scan_fd;
    int result = 0;

    scan_fd = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (scan_fd < 0)
        return -1;
    directory = fdopendir(scan_fd);
    if (!directory) {
        close(scan_fd);
        return -1;
    }
    errno = 0;
    while ((entry = readdir(directory)) != NULL) {
        struct stat state;

        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (fstatat(fd, entry->d_name, &state, AT_SYMLINK_NOFOLLOW) != 0) {
            result = -1;
            continue;
        }
        if (S_ISDIR(state.st_mode)) {
            int child = openat(fd, entry->d_name,
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            if (child < 0) {
                result = -1;
                continue;
            }
            if (clear_contents(child) != 0)
                result = -1;
            if (close(child) != 0)
                result = -1;
            if (unlinkat(fd, entry->d_name, AT_REMOVEDIR) != 0)
                result = -1;
        } else if (unlinkat(fd, entry->d_name, 0) != 0) {
            result = -1;
        }
        errno = 0;
    }
    if (errno != 0)
        result = -1;
    if (closedir(directory) != 0)
        result = -1;
    if (fsync(fd) != 0)
        result = -1;
    return result;
}

static int clear_directory(int root_fd, const char *name)
{
    int fd;
    int result;

    fd = openat(root_fd, name,
                O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    result = clear_contents(fd);
    if (close(fd) != 0)
        result = -1;
    return result;
}

int le_factory_reset_clear(const char *data_root)
{
    int root_fd;
    int result = 0;

    if (!data_root || data_root[0] != '/' || data_root[1] == '\0')
        return -1;
    root_fd = open(data_root,
                   O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0)
        return -1;
    if (clear_directory(root_fd, "config") != 0)
        result = -1;
    if (clear_directory(root_fd, "secrets") != 0)
        result = -1;
    if (close(root_fd) != 0)
        result = -1;
    return result;
}
