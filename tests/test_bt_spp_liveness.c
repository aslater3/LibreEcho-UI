/*
 * Host regression test for bt_profile.c SPP gadget session liveness and
 * wall-clock reminder pulse timing using real sockets.
 *
 * Includes bt_profile.c directly so internal SPP session state and
 * deterministic timestamps (spp_sync_alerts_at) can be exercised without
 * changing public headers or sleeping 11 seconds.
 */
#define _POSIX_C_SOURCE 200809L
#include "adapter/bt_profile.c"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static const uint8_t *find_sub(const uint8_t *buf, size_t len, const char *s)
{
    size_t sl = strlen(s);
    size_t i;

    if (sl == 0 || len < sl)
        return NULL;
    for (i = 0; i + sl <= len; ++i)
        if (memcmp(buf + i, s, sl) == 0)
            return buf + i;
    return NULL;
}

static ssize_t drain_nonblock(int fd, uint8_t *buf, size_t max)
{
    struct pollfd pfd;
    ssize_t total = 0;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    while (poll(&pfd, 1, 20) > 0 && (pfd.revents & POLLIN) &&
           (size_t)total < max) {
        ssize_t n = read(fd, buf + total, max - (size_t)total);
        if (n <= 0)
            break;
        total += n;
    }
    return total;
}

static size_t build_discover_response_frame(uint8_t *out, size_t max)
{
    static const char ns[] = "Alexa.Discovery";
    static const char nm[] = "Discover.Response";
    uint8_t msg[128];
    size_t ns_len = sizeof(ns) - 1;
    size_t nm_len = sizeof(nm) - 1;
    size_t hdr_len = 2 + ns_len + 2 + nm_len;
    size_t ev_len = 2 + hdr_len;
    size_t off = 0;

    msg[off++] = 0x0a;
    msg[off++] = (uint8_t)ev_len;
    msg[off++] = 0x0a;
    msg[off++] = (uint8_t)hdr_len;
    msg[off++] = 0x0a;
    msg[off++] = (uint8_t)ns_len;
    memcpy(msg + off, ns, ns_len);
    off += ns_len;
    msg[off++] = 0x12;
    msg[off++] = (uint8_t)nm_len;
    memcpy(msg + off, nm, nm_len);
    off += nm_len;
    return le_agt_encode(out, max, 1, msg, off);
}

/* 1. Regression for commit 9728a0b: spp_listener must start at -1, not 0. */
static int test_spp_listener_init(void)
{
    struct le_profiles p;

    CHECK(le_profile_test_init(&p, "Test") == 0);
    CHECK(p.spp_listener == -1);
    le_profile_test_cleanup(&p);
    return 0;
}

/*
 * 2. Reminder pulse timing:
 *    - When a reminder fires, `timers=active` goes out and DeleteAlert is
 *      deferred.
 *    - On the next poll (+1 s) and at +10 s, NEITHER `timers=cleared` NOR
 *      DeleteAlert is sent (pulse is not cut short).
 *    - At +11 s (`LE_GADGET_PULSE_CLEAR_SECONDS`), BOTH `timers=cleared` and
 *      deferred `DeleteAlert` arrive.
 *    - Re-arming the same reminder afterwards re-sends `SetAlert`.
 */
static int test_reminder_pulse_window(void)
{
    struct le_profiles p;
    struct le_gadget g;
    struct le_profile_sessions *sessions;
    int fds[2];
    uint8_t wire[4096];
    ssize_t n;
    char json[256];
    time_t base = 1700000000;

    le_gadget_init(&g);
    CHECK(le_profile_test_init(&p, "Test") == 0);
    p.gadget = &g;
    sessions = (struct le_profile_sessions *)p.sessions;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sessions->spp[0].fd = fds[0];
    sessions->spp[0].offered = 1;
    sessions->spp[0].answered = 1;
    sessions->spp[0].opened_at = base;
    sessions->spp[0].gadget = &g;

    /* T+0: reminder 2 armed -> SetAlert sent. */
    snprintf(json, sizeof(json),
             "{\"timers\":[{\"id\":2,\"kind\":\"reminder\","
             "\"seconds_remaining\":60,\"due_epoch\":%lld}],\"ringing\":0}",
             (long long)(base + 60));
    spp_sync_alerts_at(&p, json, base);
    n = drain_nonblock(fds[1], wire, sizeof(wire));
    CHECK(n > 0);
    CHECK(find_sub(wire, (size_t)n, "SetAlert") != NULL);

    /* T+60: reminder 2 fires (timerd speaks and immediately dismisses). */
    spp_sync_alerts_at(&p, "{\"timers\":[],\"ringing\":0}", base + 60);
    n = drain_nonblock(fds[1], wire, sizeof(wire));
    CHECK(n > 0);
    CHECK(find_sub(wire, (size_t)n, "active") != NULL);
    CHECK(find_sub(wire, (size_t)n, "cleared") == NULL);
    CHECK(find_sub(wire, (size_t)n, "DeleteAlert") == NULL);

    /* T+61 (+1 s): next poll must NOT cancel the ~10 s pulse. */
    spp_sync_alerts_at(&p, "{\"timers\":[],\"ringing\":0}", base + 61);
    CHECK(drain_nonblock(fds[1], wire, sizeof(wire)) == 0);

    /* T+70 (+10 s): still inside the 11 s pulse window -> nothing yet. */
    spp_sync_alerts_at(&p, "{\"timers\":[],\"ringing\":0}", base + 70);
    CHECK(drain_nonblock(fds[1], wire, sizeof(wire)) == 0);

    /* T+71 (+11 s = LE_GADGET_PULSE_CLEAR_SECONDS): clear + DeleteAlert arrive. */
    spp_sync_alerts_at(&p, "{\"timers\":[],\"ringing\":0}",
                       base + 60 + LE_GADGET_PULSE_CLEAR_SECONDS);
    n = drain_nonblock(fds[1], wire, sizeof(wire));
    CHECK(n > 0);
    CHECK(find_sub(wire, (size_t)n, "cleared") != NULL);
    CHECK(find_sub(wire, (size_t)n, "DeleteAlert") != NULL);

    /* Re-arming the same reminder later must not be suppressed by stale dedup. */
    spp_sync_alerts_at(&p, json, base + 80);
    n = drain_nonblock(fds[1], wire, sizeof(wire));
    CHECK(n > 0);
    CHECK(find_sub(wire, (size_t)n, "SetAlert") != NULL);

    /* Clean up alert state so later tests start empty. */
    spp_sync_alerts_at(&p, "{\"timers\":[],\"ringing\":0}", base + 90);
    (void)drain_nonblock(fds[1], wire, sizeof(wire));
    spp_sync_alerts_at(&p, "{\"timers\":[],\"ringing\":0}",
                       base + 90 + LE_GADGET_PULSE_CLEAR_SECONDS);
    (void)drain_nonblock(fds[1], wire, sizeof(wire));

    close(fds[1]);
    le_profile_test_cleanup(&p);
    return 0;
}

/*
 * 3. Countdown dismiss clears immediately (0 s delay, not held for 11 s).
 */
static int test_countdown_immediate_clear(void)
{
    struct le_profiles p;
    struct le_gadget g;
    struct le_profile_sessions *sessions;
    int fds[2];
    uint8_t wire[4096];
    ssize_t n;
    time_t base = 1700001000;

    le_gadget_init(&g);
    CHECK(le_profile_test_init(&p, "Test") == 0);
    p.gadget = &g;
    sessions = (struct le_profile_sessions *)p.sessions;

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sessions->spp[0].fd = fds[0];
    sessions->spp[0].offered = 1;
    sessions->spp[0].answered = 1;
    sessions->spp[0].opened_at = base;
    sessions->spp[0].gadget = &g;

    /* T+0: countdown ringing -> timers=active sent. */
    spp_sync_alerts_at(&p, "{\"timers\":[],\"ringing\":1}", base);
    n = drain_nonblock(fds[1], wire, sizeof(wire));
    CHECK(n > 0);
    CHECK(find_sub(wire, (size_t)n, "active") != NULL);

    /* T+1: user stops countdown (ringing -> 0) -> timers=cleared immediately. */
    spp_sync_alerts_at(&p, "{\"timers\":[],\"ringing\":0}", base + 1);
    n = drain_nonblock(fds[1], wire, sizeof(wire));
    CHECK(n > 0);
    CHECK(find_sub(wire, (size_t)n, "cleared") != NULL);

    close(fds[1]);
    le_profile_test_cleanup(&p);
    return 0;
}

/*
 * 4. Dead session reaping via read()==0, POLLHUP, POLLERR, POLLIN|POLLHUP,
 *    spp_send() pre-write liveness check, and 15 s Discover.Response timeout,
 *    plus verifying a fresh connection cleanly reopens the channel afterwards.
 */
static int test_dead_session_reaping_and_reopen(void)
{
    struct le_profiles p;
    struct le_gadget g;
    struct le_profile_sessions *sessions;
    struct pollfd pollfds[LE_PROFILE_MAX_FDS];
    int fd_map[LE_PROFILE_MAX_FDS];
    int fds[2];
    uint8_t msg[LE_AGT_FRAME_MAX];
    uint8_t frame[LE_AGT_FRAME_MAX];
    int count;
    time_t base = 1700002000;

    le_gadget_init(&g);
    CHECK(le_profile_test_init(&p, "Test") == 0);
    p.gadget = &g;
    sessions = (struct le_profile_sessions *)p.sessions;

    /* Case A: peer closes while idle -> read()==0 / POLLHUP reaps slot. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sessions->spp[0].fd = fds[0];
    sessions->spp[0].offered = 1;
    sessions->spp[0].answered = 1;
    sessions->spp[0].gadget = &g;
    close(fds[1]);

    count = le_profile_poll_setup(&p, pollfds, LE_PROFILE_MAX_FDS, fd_map);
    CHECK(count == 1);
    CHECK(poll(pollfds, 1, 200) == 1);
    le_profile_poll_events(&p, pollfds, fd_map, count);
    CHECK(sessions->spp[0].fd == -1);
    CHECK(sessions->spp[0].offered == 0);
    CHECK(sessions->spp[0].answered == 0);

    /* Case B: pure POLLHUP and pure POLLERR (without POLLIN) both reap. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sessions->spp[0].fd = fds[0];
    sessions->spp[0].offered = 1;
    sessions->spp[0].answered = 1;
    pollfds[0].fd = fds[0];
    pollfds[0].events = POLLIN;
    pollfds[0].revents = POLLHUP;
    fd_map[0] = 50;
    le_profile_poll_events(&p, pollfds, fd_map, 1);
    CHECK(sessions->spp[0].fd == -1);
    close(fds[1]);

    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sessions->spp[0].fd = fds[0];
    sessions->spp[0].offered = 1;
    sessions->spp[0].answered = 1;
    pollfds[0].fd = fds[0];
    pollfds[0].events = POLLIN;
    pollfds[0].revents = POLLERR;
    fd_map[0] = 50;
    le_profile_poll_events(&p, pollfds, fd_map, 1);
    CHECK(sessions->spp[0].fd == -1);
    close(fds[1]);

    /* Case C: POLLIN | POLLHUP together drains final frame then closes. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sessions->spp[0].fd = fds[0];
    sessions->spp[0].offered = 1;
    sessions->spp[0].answered = 0;
    {
        size_t flen = build_discover_response_frame(frame, sizeof(frame));
        CHECK(flen > 0);
        CHECK(write(fds[1], frame, flen) == (ssize_t)flen);
    }
    close(fds[1]);
    pollfds[0].fd = fds[0];
    pollfds[0].events = POLLIN;
    pollfds[0].revents = POLLIN | POLLHUP;
    fd_map[0] = 50;
    le_profile_poll_events(&p, pollfds, fd_map, 1);
    CHECK(sessions->spp[0].fd == -1);

    /* Case D: peer closes before spp_send -> returns < 0 and reaps immediately. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sessions->spp[0].fd = fds[0];
    sessions->spp[0].offered = 1;
    sessions->spp[0].answered = 1;
    sessions->spp[0].gadget = &g;
    close(fds[1]);
    CHECK(spp_send(&sessions->spp[0], msg,
                   le_agt_build_discover(msg, sizeof(msg))) < 0);
    CHECK(sessions->spp[0].fd == -1);

    /* Case E: silent peer never answers Discover -> reaped at +15 s, not +14 s. */
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    sessions->spp[0].fd = fds[0];
    sessions->spp[0].offered = 1;
    sessions->spp[0].answered = 0;
    sessions->spp[0].opened_at = base;
    sessions->spp[0].gadget = &g;

    spp_sync_alerts_at(&p, "{\"timers\":[],\"ringing\":0}",
                       base + LE_GADGET_HANDSHAKE_TIMEOUT_SECONDS - 1);
    CHECK(sessions->spp[0].fd >= 0);

    spp_sync_alerts_at(&p, "{\"timers\":[],\"ringing\":0}",
                       base + LE_GADGET_HANDSHAKE_TIMEOUT_SECONDS);
    CHECK(sessions->spp[0].fd == -1);
    close(fds[1]);

    /*
     * Case F: with all sessions reaped, a fresh incoming connection on
     * spp_listener cleanly reopens slot 0 and sends Discover + ClearIndicator.
     */
    {
        int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        int client_fd = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        uint8_t wire[1024];
        ssize_t n;

        CHECK(listen_fd >= 0 && client_fd >= 0);
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        CHECK(bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        CHECK(listen(listen_fd, 1) == 0);
        CHECK(getsockname(listen_fd, (struct sockaddr *)&addr, &alen) == 0);
        CHECK(connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

        p.spp_listener = listen_fd;
        count = le_profile_poll_setup(&p, pollfds, LE_PROFILE_MAX_FDS, fd_map);
        CHECK(count == 1 && fd_map[0] == 4);
        CHECK(poll(pollfds, 1, 200) == 1);
        le_profile_poll_events(&p, pollfds, fd_map, count);

        CHECK(sessions->spp[0].fd >= 0);
        CHECK(sessions->spp[0].offered == 1);
        CHECK(sessions->spp[0].answered == 0);
        n = drain_nonblock(client_fd, wire, sizeof(wire));
        CHECK(n > 0);
        CHECK(find_sub(wire, (size_t)n, "Alexa.Discovery") != NULL);
        CHECK(find_sub(wire, (size_t)n, "ClearIndicator") != NULL);
        close(client_fd);
    }

    le_profile_test_cleanup(&p);
    return 0;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (test_spp_listener_init() ||
        test_reminder_pulse_window() ||
        test_countdown_immediate_clear() ||
        test_dead_session_reaping_and_reopen())
        return 1;
    printf("bt spp liveness and reminder pulse contract: ok\n");
    return 0;
}
