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

/*
 * A kernel attribute can refuse a write that no host filesystem refuses: the
 * mute lamp's store() returns -EBUSY while the physical latch (or the shutdown
 * dialog) owns the line -- the request is recorded and applied when the line is
 * free -- and -EOPNOTSUPP on a board whose latch this control must not drive.
 * Arm one of those errnos here to stand in for it; 0 leaves writes alone.
 * buttond.c writes exactly one thing with write(), the lamp line, so a test that
 * arms this around a single call cannot disturb anything else. Include this
 * header before redirecting write().
 */
static int buttond_fixture_write_errno;

/*
 * A kernel store() can also consume nothing and return a short (0) write, and
 * such a return leaves errno unspecified -- on a host it may still hold whatever
 * an earlier call set. Arm this to stand in for it, with the stale errno given
 * explicitly so a test is deterministic: the caller must classify the short
 * write on its return value, never on errno. 0 disarms it.
 */
static int buttond_fixture_write_short;
static int buttond_fixture_write_short_errno;

/* static inline: a test that only redirects open() must not trip -Wunused. */
static inline ssize_t buttond_fixture_write(int fd, const void *buffer,
                                            size_t count)
{
    if (buttond_fixture_write_short) {
        errno = buttond_fixture_write_short_errno;
        return 0;
    }
    if (buttond_fixture_write_errno) {
        errno = buttond_fixture_write_errno;
        return -1;
    }
    return write(fd, buffer, count);
}

#endif
