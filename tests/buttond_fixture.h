#ifndef LIBREECHO_BUTTOND_FIXTURE_H
#define LIBREECHO_BUTTOND_FIXTURE_H

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Included before redirecting buttond.c's open(). Hardware paths never reach
 * the host: sysfs writes use anonymous scratch files, device nodes are denied.
 * This tests userspace requests only, not the physical privacy latch. */
static unsigned int buttond_fixture_opens;

static int buttond_fixture_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    buttond_fixture_opens++;

    if (!strncmp(path, "/sys/", 5)) {
        FILE *scratch = tmpfile();
        int fd;
        if (!scratch)
            return -1;
        fd = dup(fileno(scratch));
        fclose(scratch);
        return fd;
    }
    if (!strncmp(path, "/dev/", 5)) {
        errno = ENODEV;
        return -1;
    }
    if (strncmp(path, "/tmp/", 5)) {
        errno = EACCES;
        return -1;
    }
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return open(path, flags, mode);
}

#endif
