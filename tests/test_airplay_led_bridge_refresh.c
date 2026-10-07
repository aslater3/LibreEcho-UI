#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* airplayd must re-point the AirPlay sandbox's bind of ledd's socket when
 * ledd restarts. A bind pins the old inode, so without this the visualiser
 * keeps writing to a deleted socket until AirPlay is restarted.
 *
 * Mounting needs privileges that host CI does not have, so mount/umount2 are
 * replaced by a model of a file bind: the target becomes a hard link to the
 * source inode (same st_dev/st_ino, like a bind), and unmounting restores the
 * regular placeholder file the init script and airplayd create. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int fake_mount_calls, fake_umount_calls;

static int fake_mount(const char *source, const char *target, const char *type,
                      unsigned long flags, const void *data)
{
    struct stat placeholder;
    (void)type;
    (void)data;
    fake_mount_calls++;
    if (flags != MS_BIND || stat(target, &placeholder) != 0 ||
        !S_ISREG(placeholder.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    if (unlink(target) != 0 || link(source, target) != 0)
        return -1;
    return 0;
}

static int fake_umount2(const char *target, int flags)
{
    struct stat bound;
    FILE *placeholder;
    (void)flags;
    fake_umount_calls++;
    if (stat(target, &bound) != 0 || !S_ISSOCK(bound.st_mode)) {
        errno = EINVAL;
        return -1;
    }
    if (unlink(target) != 0)
        return -1;
    placeholder = fopen(target, "w");
    if (!placeholder)
        return -1;
    fclose(placeholder);
    return 0;
}

#define mount fake_mount
#define umount2 fake_umount2
#define main airplayd_program_main
#include "../src/adapter/airplayd.c"
#undef main
#undef mount
#undef umount2

static int listen_at(const char *path)
{
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    assert(strlen(path) < sizeof(address.sun_path));
    memcpy(address.sun_path, path, strlen(path) + 1);
    (void)unlink(path);
    assert(bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
    assert(listen(fd, 1) == 0);
    return fd;
}

static int same_inode(const char *a, const char *b)
{
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev &&
           sa.st_ino == sb.st_ino;
}

static void require_reaches(const char *target, int listener)
{
    struct sockaddr_un address;
    int client = socket(AF_UNIX, SOCK_STREAM, 0), served;
    assert(client >= 0);
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, target, strlen(target) + 1);
    assert(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
    served = accept(listener, NULL, NULL);
    assert(served >= 0);
    close(served);
    close(client);
}

int main(void)
{
    char root[] = "/tmp/le-airplay-led-bridge-XXXXXX";
    char source[96], sandbox_dir[96], target[128], missing[128];
    int first, second;

    assert(mkdtemp(root));
    snprintf(source, sizeof(source), "%s/led.sock", root);
    snprintf(sandbox_dir, sizeof(sandbox_dir), "%s/sandbox", root);
    snprintf(target, sizeof(target), "%s/led.sock", sandbox_dir);
    snprintf(missing, sizeof(missing), "%s/absent/led.sock", root);
    assert(mkdir(sandbox_dir, 0755) == 0);

    /* ledd not up yet: nothing to bridge, never an error. */
    assert(airplay_led_bridge_refresh(source, target) == 0);
    assert(fake_mount_calls == 0);

    first = listen_at(source);
    /* Sandbox /run not prepared yet: wait, do not create it. */
    assert(airplay_led_bridge_refresh(source, missing) == 0);
    assert(fake_mount_calls == 0);

    /* First bridge, then idempotent while current. */
    assert(airplay_led_bridge_refresh(source, target) == 1);
    assert(same_inode(source, target));
    require_reaches(target, first);
    assert(airplay_led_bridge_refresh(source, target) == 0);
    assert(fake_mount_calls == 1);

    /* ledd restarts: a new socket inode at the same path. The bridge still
     * pins the old one until refreshed. */
    close(first);
    second = listen_at(source);
    assert(!same_inode(source, target));
    assert(airplay_led_bridge_refresh(source, target) == 1);
    assert(fake_umount_calls >= 1);
    assert(same_inode(source, target));
    require_reaches(target, second);
    assert(airplay_led_bridge_refresh(source, target) == 0);
    assert(fake_mount_calls == 2);

    close(second);
    unlink(source);
    unlink(target);
    rmdir(sandbox_dir);
    rmdir(root);
    puts("airplay LED bridge follows ledd restarts: ok");
    return 0;
}
