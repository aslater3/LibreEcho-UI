#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

/* Exercise the real upload worker, redirecting every reachable device file
 * operation to TMPDIR. Stop after promotion, before invoking an installer. */
static char stage[1024];
static int fail_rename;
static const char *fixture_path(const char *path)
{
    static char mapped[1200];
    const char *name = strrchr(path, '/');
    assert(name && !strncmp(path, "/data/libreecho/", 16));
    assert(snprintf(mapped, sizeof(mapped), "%s%s", stage, name) < (int)sizeof(mapped));
    return mapped;
}
static int fixture_mkdir(const char *path, mode_t mode)
{
    (void)mode;
    assert(!strcmp(path, "/data/libreecho") ||
           !strcmp(path, "/data/libreecho/update") ||
           !strcmp(path, "/data/libreecho/update/incoming"));
    errno = EEXIST;
    return -1;
}
static int fixture_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = (mode_t)va_arg(args, int);
        va_end(args);
    }
    return open(fixture_path(path), flags, mode);
}
static int fixture_unlink(const char *path)
{
    return unlink(fixture_path(path));
}
static int fixture_rename(const char *from, const char *to)
{
    char source[1200];
    int fd;
    if (fail_rename) { errno = EIO; return -1; }
    snprintf(source, sizeof(source), "%s", fixture_path(from));
    if (rename(source, fixture_path(to))) return -1;
    /* A new staging file must not be removed after ownership was consumed. */
    fd = open(source, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(write(fd, "replacement", 11) == 11);
    close(fd);
    return 0;
}
static int fixture_mkstemp(char *path)
{
    assert(strstr(path, "/data/libreecho/update/incoming/.update-error-") == path);
    errno = EACCES;
    return -1;
}
#define mkdir fixture_mkdir
#define open fixture_open
#define unlink fixture_unlink
#define rename fixture_rename
#define mkstemp fixture_mkstemp
#include "../src/http_server.c"
#undef mkdir
#undef open
#undef unlink
#undef rename
#undef mkstemp

static void create_file(const char *name, const char *body)
{
    char path[1200];
    int fd;
    snprintf(path, sizeof(path), "%s/%s", stage, name);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(fd >= 0);
    assert(write(fd, body, strlen(body)) == (ssize_t)strlen(body));
    close(fd);
}
static void check_file(const char *name, const char *expected)
{
    char path[1200], body[64] = {0};
    int fd;
    snprintf(path, sizeof(path), "%s/%s", stage, name);
    fd = open(path, O_RDONLY);
    if (!expected) { assert(fd < 0 && errno == ENOENT); return; }
    assert(fd >= 0);
    assert(read(fd, body, sizeof(body)-1) == (ssize_t)strlen(expected));
    close(fd);
    assert(!strcmp(body, expected));
}
static void remove_file(const char *name)
{
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", stage, name);
    assert(!unlink(path));
}
static void upload(const char *initial, size_t length, size_t total, const char *status)
{
    int pair[2];
    char response_body[1024] = {0};
    assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    assert(!shutdown(pair[1], SHUT_WR));
    assert(!stream_update_upload(pair[0], initial, length, total, 0));
    assert(read(pair[1], response_body, sizeof(response_body)-1) > 0);
    assert(strstr(response_body, status));
    close(pair[1]);
}
int main(void)
{
    const char *tmp = getenv("TMPDIR");
    assert(tmp && *tmp);
    assert(snprintf(stage, sizeof(stage), "%s/le-upload-XXXXXX", tmp) < (int)sizeof(stage));
    assert(mkdtemp(stage));

    create_file("upload.lock", "owner");
    create_file("manual.tar.tmp", "active upload");
    upload("", 0, 1, "409");
    check_file("upload.lock", "owner");
    check_file("manual.tar.tmp", "active upload");
    remove_file("upload.lock");

    /* Acquiring the lock does not confer ownership of a pre-existing tmp. */
    upload("", 0, 1, "503");
    check_file("upload.lock", NULL);
    check_file("manual.tar.tmp", "active upload");
    remove_file("manual.tar.tmp");

    upload("partial", 7, 8, "503");
    check_file("upload.lock", NULL);
    check_file("manual.tar.tmp", NULL);

    fail_rename = 1;
    upload("package", 7, 7, "503");
    check_file("upload.lock", NULL);
    check_file("manual.tar.tmp", NULL);

    fail_rename = 0;
    upload("package", 7, 7, "503");
    check_file("upload.lock", NULL);
    check_file("manual.tar", "package");
    check_file("manual.tar.tmp", "replacement");
    remove_file("manual.tar");
    remove_file("manual.tar.tmp");
    assert(!rmdir(stage));
    puts("OTA upload cleanup ownership: ok");
    return 0;
}
