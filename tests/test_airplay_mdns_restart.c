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
    /* ...but not before the new responder has settled, */
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

    /* A replaced responder must settle before Shairport Sync is started
     * against it: Avahi has a pid before it owns its bus name. */
    {
        struct airplay_ctx ctx;
        memset(&ctx, 0, sizeof(ctx));
        write_pid(pidfile, 103);
        airplay_mdns_generation_read(bus, pidfile, &live);
        assert(!airplay_mdns_settled(&ctx, &live, 1, 10000));
        assert(!airplay_mdns_settled(&ctx, &live, 1, 10000 + AIRPLAY_MDNS_SETTLE_MS - 1));
        assert(airplay_mdns_settled(&ctx, &live, 1, 10000 + AIRPLAY_MDNS_SETTLE_MS));
        /* not while the supervisor reports degraded */
        assert(!airplay_mdns_settled(&ctx, &live, 0, 20000));
        /* another respawn restarts the settle window */
        write_pid(pidfile, 104);
        airplay_mdns_generation_read(bus, pidfile, &live);
        assert(!airplay_mdns_settled(&ctx, &live, 1, 20000));
        assert(airplay_mdns_settled(&ctx, &live, 1, 20000 + AIRPLAY_MDNS_SETTLE_MS));

        /* Shairport Sync exiting (no Avahi yet) is retried with backoff,
         * reset after a healthy run, never a reason to drop AirPlay. */
        ctx.shairport_started_ms = 1000;
        airplay_shairport_exited(&ctx, 1500);
        assert(ctx.shairport_backoff_ms == SHAIRPORT_BACKOFF_MIN_MS);
        assert(ctx.shairport_retry_ms == 1500 + SHAIRPORT_BACKOFF_MIN_MS);
        ctx.shairport_started_ms = 4000;
        airplay_shairport_exited(&ctx, 4500);
        assert(ctx.shairport_backoff_ms == 2 * SHAIRPORT_BACKOFF_MIN_MS);
        for (int i = 0; i < 8; ++i)
            airplay_shairport_exited(&ctx, 4600);
        assert(ctx.shairport_backoff_ms == SHAIRPORT_BACKOFF_MAX_MS);
        ctx.shairport_started_ms = 5000;
        airplay_shairport_exited(&ctx, 5000 + SHAIRPORT_HEALTHY_RUN_MS);
        assert(ctx.shairport_backoff_ms == SHAIRPORT_BACKOFF_MIN_MS);
    }

    close(second);
    unlink(pidfile);
    unlink(bus);
    rmdir(root);
    puts("airplay re-registers after a shared mDNS restart: ok");
    return 0;
}
