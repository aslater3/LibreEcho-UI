#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

/* When libreecho-mdnsd replaces the shared D-Bus or Avahi daemon (a watchdog
 * restart, a crashed child respawned), Shairport Sync's Avahi client is left
 * on the dead instance and AirPlay stops being advertised for good.
 * airplayd must notice the new instance and restart Shairport Sync once the
 * responder runs again, but never cut off a stream that is playing. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define main airplayd_program_main
#include "../src/adapter/airplayd.c"
#undef main

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

static void write_pid(const char *path, long pid)
{
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "%ld\n", pid);
    fclose(f);
}

int main(void)
{
    char root[] = "/tmp/le-airplay-mdns-XXXXXX";
    char bus[96], pidfile[96];
    struct airplay_mdns_generation registered, live;
    int first, second;

    assert(mkdtemp(root));
    snprintf(bus, sizeof(bus), "%s/system_bus_socket", root);
    snprintf(pidfile, sizeof(pidfile), "%s/avahi.pid", root);

    /* Nothing running yet: an empty generation, and never a restart. */
    airplay_mdns_generation_read(bus, pidfile, &live);
    assert(!live.bus_ino && !live.avahi_pid);
    memset(&registered, 0, sizeof(registered));
    assert(!airplay_mdns_restart_due(&registered, &live, 0, 0));

    first = listen_at(bus);
    write_pid(pidfile, 100);
    airplay_mdns_generation_read(bus, pidfile, &registered);
    assert(registered.bus_ino && registered.avahi_pid == 100);

    /* The instance Shairport registered with is still live. */
    airplay_mdns_generation_read(bus, pidfile, &live);
    assert(!airplay_mdns_restart_due(&registered, &live, 1, 0));

    /* Avahi alone respawned on the same bus. */
    write_pid(pidfile, 101);
    airplay_mdns_generation_read(bus, pidfile, &live);
    assert(airplay_mdns_restart_due(&registered, &live, 1, 0));
    /* ...but not while the responder is still coming up, */
    assert(!airplay_mdns_restart_due(&registered, &live, 0, 0));
    /* ...and not in the middle of a stream. */
    assert(!airplay_mdns_restart_due(&registered, &live, 1, 1));

    /* The whole stack restarted: a new bus socket inode and a new Avahi. */
    registered = live;
    close(first);
    second = listen_at(bus);
    write_pid(pidfile, 102);
    airplay_mdns_generation_read(bus, pidfile, &live);
    assert(live.bus_ino && live.avahi_pid == 102);
    assert(airplay_mdns_restart_due(&registered, &live, 1, 0));

    /* Avahi is gone (no pidfile yet): wait for it rather than restart into
     * the same failure. */
    unlink(pidfile);
    airplay_mdns_generation_read(bus, pidfile, &live);
    assert(!live.avahi_pid);
    assert(!airplay_mdns_restart_due(&registered, &live, 1, 0));

    close(second);
    unlink(bus);
    rmdir(root);
    puts("airplay re-registers after a shared mDNS restart: ok");
    return 0;
}
