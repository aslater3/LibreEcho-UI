/*
 * Transport regression for the asynchronous kernel recovery scan (UI #288).
 *
 * While the recovery AP owns the radio the scan can only run through nl80211,
 * and its completion is multicast: NEW_SCAN_RESULTS / SCAN_ABORTED are delivered
 * only to a socket that resolved the family's "scan" group from
 * CTRL_ATTR_MCAST_GROUPS and subscribed with NETLINK_ADD_MEMBERSHIP before
 * TRIGGER_SCAN.  The host has no radio, so this fixture links the real daemon
 * source and exercises the transport where the review demanded it:
 *
 *   - nested CTRL_CMD_GETFAMILY reply parsing on kernel-shaped bytes,
 *     including the doubly nested group list, name-length forms, truncation,
 *     sequence mismatch, and kernel errors;
 *   - a live kernel round trip: resolve nl80211 (with nlctrl as a portable
 *     fallback), parse the real nested reply with the production parser, and
 *     subscribe to a real multicast group with the production join helper;
 *   - scan completion-event and dump-response parsing over kernel-shaped
 *     datagrams, including acknowledgement/error interleaving and truncation;
 *   - the asynchronous dispatch: driver_scan_begin() returns without blocking,
 *     and the daemon's own loop (check_scan_timeout) completes the waiting
 *     client before the bounded deadline, and with the timeout error at it.
 *
 * A real radio scan and kernel-originated multicast delivery remain a
 * hardware gate; nothing here claims them.
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
static unsigned char sent_request[4096];
static size_t sent_length;
static int capture_send;
static ssize_t transport_sendto(int fd, const void *buf, size_t len, int flags,
                                const struct sockaddr *dest, socklen_t addrlen)
{
    if (!capture_send) return sendto(fd, buf, len, flags, dest, addrlen);
    if (len > sizeof(sent_request)) { errno = EMSGSIZE; return -1; }
    memcpy(sent_request, buf, len);
    sent_length = len;
    return (ssize_t)len;
}
#define sendto transport_sendto
#define main libreecho_networkd_main
#include "../src/adapter/networkd.c"
#undef main
#undef sendto

#include <sys/socket.h>
#include <sys/un.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static size_t append_group(unsigned char *buffer, size_t capacity, size_t used,
                           const char *name, uint32_t id, int name_nul)
{
    size_t group_start = used;
    struct nlattr *group;
    size_t name_length = strlen(name);

    group = (struct nlattr *)(buffer + used);
    group->nla_type = 1; /* the kernel nests groups under an unnamed type */
    group->nla_len = NLA_HDRLEN;
    used += NLA_ALIGN(NLA_HDRLEN);
    if (nl_put(buffer, capacity, &used, CTRL_ATTR_MCAST_GRP_NAME, name,
               name_length + (name_nul ? 1 : 0)) < 0)
        return 0;
    if (nl_put_u32(buffer, capacity, &used, CTRL_ATTR_MCAST_GRP_ID, id) < 0)
        return 0;
    group = (struct nlattr *)(buffer + group_start);
    group->nla_len = (uint16_t)(used - group_start);
    return used;
}

/* Build a CTRL_CMD_GETFAMILY reply the way the kernel lays it out: family id
 * and name at the top level, and the multicast groups nested twice. */
static size_t build_family_reply(unsigned char *buffer, size_t capacity,
                                 uint16_t family, int groups_before_scan,
                                 int include_scan_group, int scan_name_nul)
{
    struct nlmsghdr *header = (struct nlmsghdr *)buffer;
    struct genlmsghdr *generic;
    size_t used = NLMSG_LENGTH(GENL_HDRLEN);
    size_t groups_start;
    struct nlattr *groups;

    memset(buffer, 0, capacity);
    header->nlmsg_type = GENL_ID_CTRL;
    header->nlmsg_flags = 0;
    header->nlmsg_seq = NL80211_FAMILY_SEQ;
    generic = (struct genlmsghdr *)NLMSG_DATA(header);
    generic->cmd = CTRL_CMD_NEWFAMILY;
    generic->version = 1;
    if (nl_put(buffer, capacity, &used, CTRL_ATTR_FAMILY_ID, &family,
               sizeof(family)) < 0)
        return 0;
    if (nl_put(buffer, capacity, &used, CTRL_ATTR_FAMILY_NAME, "nl80211",
               sizeof("nl80211")) < 0)
        return 0;
    groups_start = used;
    groups = (struct nlattr *)(buffer + used);
    groups->nla_type = CTRL_ATTR_MCAST_GROUPS;
    groups->nla_len = NLA_HDRLEN;
    used += NLA_ALIGN(NLA_HDRLEN);
    if (groups_before_scan) {
        used = append_group(buffer, capacity, used, "vendor", 9, 1);
        if (!used)
            return 0;
    }
    if (include_scan_group) {
        used = append_group(buffer, capacity, used, "scan", 7, scan_name_nul);
        if (!used)
            return 0;
    }
    groups = (struct nlattr *)(buffer + groups_start);
    groups->nla_len = (uint16_t)(used - groups_start);
    header->nlmsg_len = (uint32_t)used;
    return used;
}

static size_t build_genl_event(unsigned char *buffer, size_t capacity,
                               uint16_t family, uint8_t command)
{
    struct nlmsghdr *header = (struct nlmsghdr *)buffer;
    struct genlmsghdr *generic;

    memset(buffer, 0, NLMSG_LENGTH(GENL_HDRLEN));
    header->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    header->nlmsg_type = family;
    header->nlmsg_flags = 0;
    header->nlmsg_seq = 0; /* multicast events carry sequence 0 */
    generic = (struct genlmsghdr *)NLMSG_DATA(header);
    generic->cmd = command;
    generic->version = 1;
    (void)capacity;
    return header->nlmsg_len;
}

static size_t build_ack(unsigned char *buffer, size_t capacity, int error,
                        uint32_t sequence)
{
    struct nlmsghdr *header = (struct nlmsghdr *)buffer;
    struct nlmsgerr *payload;
    size_t used = NLMSG_LENGTH(sizeof(*payload));

    memset(buffer, 0, capacity < used ? capacity : used);
    header->nlmsg_len = (uint32_t)used;
    header->nlmsg_type = NLMSG_ERROR;
    header->nlmsg_flags = 0;
    header->nlmsg_seq = sequence;
    payload = (struct nlmsgerr *)NLMSG_DATA(header);
    payload->error = error;
    return used;
}

/* Build one dump message carrying NL80211_ATTR_BSS for `ssid` at `frequency`,
 * optionally with a WPA2-PSK RSN information element. */
static size_t build_dump_message(unsigned char *buffer, size_t offset,
                                 size_t capacity, uint16_t family,
                                 const char *ssid, uint32_t frequency)
{
    static const unsigned char rsn[] = {
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x04,
        0x01, 0x00, 0x00, 0x0f, 0xac, 0x02
    };
    unsigned char ies[64];
    size_t ies_length = 0;
    size_t bss_start, used;
    struct nlmsghdr *header = (struct nlmsghdr *)(buffer + offset);
    struct genlmsghdr *generic;
    struct nlattr *bss;
    int32_t signal_mbm = -4200;
    unsigned char bssid[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    used = offset + NLMSG_LENGTH(GENL_HDRLEN);
    header->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    header->nlmsg_type = family;
    header->nlmsg_flags = 0;
    header->nlmsg_seq = NL80211_DUMP_SEQ;
    generic = (struct genlmsghdr *)NLMSG_DATA(header);
    generic->cmd = NL80211_CMD_NEW_SCAN_RESULTS;
    generic->version = 1;

    /* Information elements: SSID then RSN (WPA2-PSK). */
    ies[ies_length++] = 0;
    ies[ies_length++] = (unsigned char)strlen(ssid);
    memcpy(ies + ies_length, ssid, strlen(ssid));
    ies_length += strlen(ssid);
    ies[ies_length++] = 48;
    ies[ies_length++] = (unsigned char)sizeof(rsn);
    memcpy(ies + ies_length, rsn, sizeof(rsn));
    ies_length += sizeof(rsn);

    bss_start = used;
    bss = (struct nlattr *)(buffer + used);
    bss->nla_type = NL80211_ATTR_BSS;
    bss->nla_len = NLA_HDRLEN;
    used += NLA_ALIGN(NLA_HDRLEN);
    if (nl_put(buffer, capacity, &used, NL80211_BSS_BSSID, bssid,
               sizeof(bssid)) < 0)
        return 0;
    if (nl_put_u32(buffer, capacity, &used, NL80211_BSS_FREQUENCY,
                   frequency) < 0)
        return 0;
    if (nl_put(buffer, capacity, &used, NL80211_BSS_SIGNAL_MBM,
               &signal_mbm, sizeof(signal_mbm)) < 0)
        return 0;
    if (nl_put(buffer, capacity, &used, NL80211_BSS_INFORMATION_ELEMENTS,
               ies, ies_length) < 0)
        return 0;
    bss = (struct nlattr *)(buffer + bss_start);
    bss->nla_len = (uint16_t)(used - bss_start);

    header->nlmsg_len = (uint32_t)(used - offset);
    return used;
}

static size_t append_done(unsigned char *buffer, size_t offset, size_t capacity)
{
    struct nlmsghdr *header;

    if (offset + NLMSG_HDRLEN > capacity)
        return 0;
    header = (struct nlmsghdr *)(buffer + offset);
    memset(header, 0, NLMSG_HDRLEN);
    header->nlmsg_len = NLMSG_HDRLEN;
    header->nlmsg_type = NLMSG_DONE;
    return offset + NLMSG_HDRLEN;
}

/* Round-trip a control request through the live kernel and parse the reply
 * with the production parser; returns 0 when family and group were resolved. */
static int resolve_live_family(const char *family_name, const char *group_name,
                               uint16_t *family_id, uint32_t *group_id)
{
    unsigned char request[NL80211_BUFFER_SIZE];
    struct sockaddr_nl address;
    struct pollfd descriptor;
    size_t used;
    ssize_t received;
    int fd;

    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0)
        return -1;
    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    used = nl80211_build_family_request(request, sizeof(request), family_name);
    if (!used || nl80211_send_kernel(fd, request, used) < 0) {
        close(fd);
        return -1;
    }
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    if (poll(&descriptor, 1, 2000) <= 0) {
        close(fd);
        return -1;
    }
    received = recv(fd, request, sizeof(request), 0);
    if (received <= 0 ||
        nl80211_parse_family_reply(request, (size_t)received,
                                   NL80211_FAMILY_SEQ, group_name, family_id,
                                   group_id) < 0 || !*family_id || !*group_id) {
        close(fd);
        return -1;
    }
    if (nl80211_join_group(fd, *group_id) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int test_family_reply_parsing(void)
{
    unsigned char buffer[4096];
    uint16_t family;
    uint32_t group;
    size_t used;

    /* Kernel-shaped reply: a group list with a non-matching container before
     * the "scan" container must still resolve both ids. */
    used = build_family_reply(buffer, sizeof(buffer), 41, 1, 1, 1);
    CHECK(used > 0);
    CHECK(nl80211_parse_family_reply(buffer, used, NL80211_FAMILY_SEQ, "scan",
                                     &family, &group) == 0);
    CHECK(family == 41 && group == 7);

    /* A name without the trailing NUL is the same group. */
    used = build_family_reply(buffer, sizeof(buffer), 42, 0, 1, 0);
    CHECK(nl80211_parse_family_reply(buffer, used, NL80211_FAMILY_SEQ, "scan",
                                     &family, &group) == 0);
    CHECK(family == 42 && group == 7);

    /* No scan group: the family is found but the caller must subscribe. */
    group = 99;
    used = build_family_reply(buffer, sizeof(buffer), 43, 1, 0, 1);
    CHECK(nl80211_parse_family_reply(buffer, used, NL80211_FAMILY_SEQ, "scan",
                                     &family, &group) == 0);
    CHECK(family == 43 && group == 0);

    /* A datagram cut mid-tail: the family is still usable, no group is
     * claimed, and nothing is read past the datagram. */
    used = build_family_reply(buffer, sizeof(buffer), 44, 1, 1, 1);
    CHECK(used > 20);
    ((struct nlmsghdr *)buffer)->nlmsg_len = (uint32_t)(used - 10);
    CHECK(nl80211_parse_family_reply(buffer, used - 10, NL80211_FAMILY_SEQ,
                                     "scan", &family, &group) == 0);
    CHECK(family == 44 && group == 0);

    /* A group container whose claimed length runs past its truncated parent
     * list is bounded the same way. */
    used = build_family_reply(buffer, sizeof(buffer), 46, 0, 1, 1);
    {
        struct nlattr *groups = (struct nlattr *)(buffer +
            NLMSG_LENGTH(GENL_HDRLEN) + 8 + 12);
        CHECK(groups->nla_len > NLA_HDRLEN + 10);
        groups->nla_len -= 10;
    }
    CHECK(nl80211_parse_family_reply(buffer, used, NL80211_FAMILY_SEQ, "scan",
                                     &family, &group) == 0);
    CHECK(family == 46 && group == 0);

    /* A different sequence is not this reply. */
    used = build_family_reply(buffer, sizeof(buffer), 45, 0, 1, 1);
    CHECK(nl80211_parse_family_reply(buffer, used, 7u, "scan", &family,
                                     &group) < 0 && errno == EPROTO);

    /* A kernel error reply reports the kernel error. */
    used = build_ack(buffer, sizeof(buffer), -ENOENT, NL80211_FAMILY_SEQ);
    CHECK(nl80211_parse_family_reply(buffer, used, NL80211_FAMILY_SEQ, "scan",
                                     &family, &group) < 0);
    CHECK(errno == ENOENT);

    puts("nl80211 nested family/group reply parsing: ok");
    return 0;
}

static int test_live_multicast_subscription(void)
{
    uint16_t family = 0;
    uint32_t group = 0;
    const char *used_family = "nl80211";
    int rc;

    rc = resolve_live_family("nl80211", "scan", &family, &group);
    if (rc < 0) {
        /* Containers and radios-less CI images may not load cfg80211; nlctrl
         * is always present and proves the same parse+subscribe path. */
        used_family = "nlctrl";
        family = 0;
        group = 0;
        rc = resolve_live_family("nlctrl", "notify", &family, &group);
    }
    CHECK(rc == 0);
    CHECK(family != 0);
    CHECK(group != 0);

    /* Producing an out-of-range subscription through the production helper
     * must fail, not silently succeed. */
    {
        int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
        CHECK(fd >= 0);
        errno = 0;
        CHECK(nl80211_join_group(fd, 0) < 0 && errno == EPROTO);
        errno = 0;
        CHECK(nl80211_join_group(fd, 0xFFF1) < 0);
        close(fd);
    }

    printf("nl80211 live %s family=%u group=%u subscription: ok\n",
           used_family, (unsigned)family, (unsigned)group);
    return 0;
}

static int test_scan_event_parsing(void)
{
    unsigned char buffer[512];
    size_t used;

    used = build_genl_event(buffer, sizeof(buffer), 41,
                            NL80211_CMD_NEW_SCAN_RESULTS);
    CHECK(nl80211_parse_scan_event(buffer, used) == 1);

    used = build_genl_event(buffer, sizeof(buffer), 41,
                            NL80211_CMD_SCAN_ABORTED);
    CHECK(nl80211_parse_scan_event(buffer, used) < 0 && errno == ECANCELED);

    /* Unrelated events are not a completion. */
    used = build_genl_event(buffer, sizeof(buffer), 41, 0x7f);
    CHECK(nl80211_parse_scan_event(buffer, used) == 0);

    /* The trigger acknowledgement and its EBUSY form are not events. */
    used = build_ack(buffer, sizeof(buffer), 0, NL80211_TRIGGER_SEQ);
    CHECK(nl80211_parse_scan_event(buffer, used) == 0);
    used = build_ack(buffer, sizeof(buffer), -EBUSY, NL80211_TRIGGER_SEQ);
    CHECK(nl80211_parse_scan_event(buffer, used) == 0);

    /* A rejected trigger is an error... */
    used = build_ack(buffer, sizeof(buffer), -EPERM, NL80211_TRIGGER_SEQ);
    CHECK(nl80211_parse_scan_event(buffer, used) < 0 && errno == EPERM);

    /* ...and an acknowledgement plus the event in one datagram still reports
     * the completion. */
    used = build_ack(buffer, sizeof(buffer), 0, NL80211_TRIGGER_SEQ);
    used += build_genl_event(buffer + used, sizeof(buffer) - used, 41,
                             NL80211_CMD_NEW_SCAN_RESULTS);
    CHECK(nl80211_parse_scan_event(buffer, used) == 1);

    puts("nl80211 scan completion-event parsing: ok");
    return 0;
}

static int test_dump_message_parsing(void)
{
    unsigned char buffer[4096];
    struct scan_result results[SCAN_MAX];
    int count = 0, done = 0;
    size_t used;

    memset(results, 0, sizeof(results));
    used = build_dump_message(buffer, 0, sizeof(buffer), 41, "RecoveryNet",
                              2412);
    CHECK(used > 0);
    CHECK(nl80211_parse_dump_message(results, &count, buffer, used, &done) == 0);
    CHECK(count == 1 && !done);
    CHECK(!strcmp(results[0].ssid, "RecoveryNet"));
    CHECK(results[0].channel == 1);
    CHECK(results[0].rssi_dbm == -42);
    CHECK(strstr(results[0].flags, "WPA2-PSK") != NULL);

    /* A second datagram continues the same dump and finishes it. */
    used = build_dump_message(buffer, 0, sizeof(buffer), 41, "SecondNet", 5180);
    CHECK(used > 0);
    used = append_done(buffer, used, sizeof(buffer));
    CHECK(used > 0);
    CHECK(nl80211_parse_dump_message(results, &count, buffer, used, &done) == 0);
    CHECK(count == 2 && done);
    CHECK(!strcmp(results[1].ssid, "SecondNet"));
    CHECK(results[1].five_ghz == 1);

    /* A not-ready dump error is retryable... */
    used = build_ack(buffer, sizeof(buffer), -EBUSY, NL80211_DUMP_SEQ);
    CHECK(nl80211_parse_dump_message(results, &count, buffer, used, &done) < 0);
    CHECK(errno == EBUSY);

    /* ...and a late trigger acknowledgement is not a dump error. */
    used = build_ack(buffer, sizeof(buffer), 0, NL80211_TRIGGER_SEQ);
    CHECK(nl80211_parse_dump_message(results, &count, buffer, used, &done) == 0);
    CHECK(count == 2 && !done);

    /* A truncated BSS attribute is skipped without touching the result ring. */
    used = build_dump_message(buffer, 0, sizeof(buffer), 41, "Truncated", 2412);
    CHECK(used > 0);
    used -= 8;
    ((struct nlmsghdr *)buffer)->nlmsg_len = (uint32_t)used;
    CHECK(nl80211_parse_dump_message(results, &count, buffer, used, &done) == 0);
    CHECK(count == 2 && !done);

    puts("nl80211 dump datagram parsing: ok");
    return 0;
}

static int test_async_dispatch_is_bounded(void)
{
    struct daemon_ctx ctx;
    char oracle_path[256];
    char reply[LE_ADAPTER_MSG_MAX];
    const char *tmpdir = getenv("TMPDIR");
    long long started, elapsed;
    int pair[2];
    FILE *file;

    if (!tmpdir || !tmpdir[0])
        tmpdir = "/tmp";
    snprintf(oracle_path, sizeof(oracle_path), "%s/le-scan-oracle-%ld.txt",
             tmpdir, (long)getpid());
    file = fopen(oracle_path, "w");
    CHECK(file != NULL);
    fputs("bssid / frequency / signal level / flags / ssid\n"
          "00:11:22:33:44:55\t2412\t-42\t[WPA2-PSK-CCMP][ESS]\tRecoveryNet\n",
          file);
    fclose(file);
    setenv("LIBREECHO_NETWORKD_SCAN_ORACLE", oracle_path, 1);
    setenv("LIBREECHO_NETWORKD_SCAN_ORACLE_DELAY_MS", "250", 1);
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);

    memset(&ctx, 0, sizeof(ctx));
    ctx.scan.fd = -1;
    /* Host fixtures answer through the oracle; the engine must not block the
     * caller while doing any of it. */
    CHECK(driver_scan_begin(&ctx) == 0);
    CHECK(ctx.scan.driver == 1 && ctx.scan.state == DRIVER_SCAN_ORACLE);
    CHECK(ctx.scan.fd == -1);

    ctx.clients[0].fd = pair[0];
    ctx.scan.active = 1;
    ctx.scan.client_fd = pair[0];
    ctx.scan.id = 77;
    ctx.scan.deadline = monotonic_ms() + NL80211_SCAN_TIMEOUT_MS;
    ctx.scan.poll_at = monotonic_ms() + scan_oracle_delay_ms();

    started = monotonic_ms();
    while (ctx.scan.active && monotonic_ms() - started < 3000) {
        if (monotonic_ms() >= ctx.scan.poll_at)
            check_scan_timeout(&ctx);
        (void)poll(NULL, 0, 5);
    }
    elapsed = monotonic_ms() - started;
    CHECK(!ctx.scan.active);
    CHECK(ctx.scan.driver == 0 && ctx.scan.state == DRIVER_SCAN_NONE);
    CHECK(ctx.scan.buffer == NULL && ctx.scan.client_fd == -1);

    /* The reply is already waiting on the client socket. */
    {
        memset(reply, 0, sizeof(reply));
        CHECK(read(pair[1], reply, sizeof(reply) - 1) > 0);
    }
    CHECK(strstr(reply, "\"ok\":true") != NULL);
    CHECK(strstr(reply, "RecoveryNet") != NULL);
    /* The daemon completed the scan on its own timer, not synchronously, and
     * well inside a bounded budget. */
    CHECK(elapsed >= 200);
    CHECK(elapsed < 2000);

    /* Without a completion the deadline produces a bounded timeout reply. */
    memset(&ctx, 0, sizeof(ctx));
    ctx.scan.fd = -1;
    CHECK(driver_scan_begin(&ctx) == 0);
    ctx.clients[0].fd = pair[0];
    ctx.scan.active = 1;
    ctx.scan.client_fd = pair[0];
    ctx.scan.id = 78;
    ctx.scan.deadline = monotonic_ms() + 60;
    ctx.scan.poll_at = ctx.scan.deadline + 500; /* oracle must not win */
    started = monotonic_ms();
    while (ctx.scan.active && monotonic_ms() - started < 1000) {
        check_scan_timeout(&ctx);
        (void)poll(NULL, 0, 5);
    }
    elapsed = monotonic_ms() - started;
    CHECK(!ctx.scan.active);
    CHECK(elapsed < 900);
    memset(reply, 0, sizeof(reply));
    CHECK(read(pair[1], reply, sizeof(reply) - 1) > 0);
    CHECK(strstr(reply, "\"ok\":false") != NULL);
    CHECK(strstr(reply, "timed out") != NULL);

    close(pair[0]);
    close(pair[1]);
    unsetenv("LIBREECHO_NETWORKD_SCAN_ORACLE");
    unsetenv("LIBREECHO_NETWORKD_SCAN_ORACLE_DELAY_MS");
    unlink(oracle_path);
    puts("networkd asynchronous driver-scan dispatch: ok");
    return 0;
}

static int test_trigger_request_header(void)
{
    struct daemon_ctx ctx;
    unsigned char buffer[NL80211_BUFFER_SIZE];
    struct nlmsghdr *header;
    struct genlmsghdr *generic;
    memset(&ctx, 0, sizeof(ctx));
    ctx.scan.buffer = buffer;
    ctx.scan.family = 41;
    ctx.scan.ifindex = 7;
    ctx.recovery.net_configured = 1;
    capture_send = 1;
    CHECK(nl80211_send_trigger_scan(&ctx) == 0);
    capture_send = 0;
    header = (struct nlmsghdr *)sent_request;
    generic = (struct genlmsghdr *)NLMSG_DATA(header);
    CHECK(header->nlmsg_len == sent_length);
    CHECK(header->nlmsg_type == 41);
    CHECK(header->nlmsg_seq == NL80211_TRIGGER_SEQ);
    CHECK((header->nlmsg_flags & (NLM_F_REQUEST | NLM_F_ACK)) == (NLM_F_REQUEST | NLM_F_ACK));
    CHECK(generic->cmd == NL80211_CMD_TRIGGER_SCAN && generic->version == 1);
    puts("nl80211 production trigger header: ok");
    return 0;
}

int main(void)
{
    CHECK(test_trigger_request_header() == 0);
    if (test_family_reply_parsing() ||
        test_live_multicast_subscription() ||
        test_scan_event_parsing() ||
        test_dump_message_parsing() ||
        test_async_dispatch_is_bounded())
        return 1;
    puts("nl80211 scan transport regression: all paths ok");
    return 0;
}
