/*
 * LibreEcho network companion daemon.
 *
 * This file deliberately speaks the wpa_supplicant control protocol directly;
 * the target image does not carry libwpa_client, libnl, NetworkManager, or
 * dbus.  The daemon is single threaded.  The only child process it creates is
 * the short-lived BusyBox udhcpc process used for DHCP.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "adapter.h"
#include "gateway_probe.h"
#include "log.h"
#include "network_health.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/genetlink.h>
#include <linux/nl80211.h>
#define _LINUX_IF_H
#ifndef IFNAMSIZ
#define IFNAMSIZ 16
#endif
#include <linux/wireless.h>
#undef _LINUX_IF_H
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define CLIENT_MAX 4
#define INPUT_MAX LE_ADAPTER_MSG_MAX
#define WPA_REPLY_MAX 8192
#define SCAN_MAX 48
#define SCAN_OUTPUT_MAX 12
#define DHCP_TIMEOUT_MS 90000
#define SCAN_TIMEOUT_MS 12000
#define SCAN_POLL_MS 1800
#define ASSOCIATION_POLL_MS 100
#define WPA_TIMEOUT_MS 2500
#define WEXT_SCAN_BUFFER_SIZE 65535
#define WEXT_SCAN_RETRY_MS 100
#define NL80211_SCAN_TIMEOUT_MS 12000
#define NL80211_SCAN_RETRY_MS 150
#define NL80211_BUFFER_SIZE 65536
#ifdef LE_NETWORKD_TESTING
#define NETWORKD_POLL_MAX_MS 5
#define ASSOCIATION_TIMEOUT_MS 300
#else
#define NETWORKD_POLL_MAX_MS 1000
#define ASSOCIATION_TIMEOUT_MS 15000
#endif

struct wpa_ctrl {
    int fd;
    char local_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
};

struct wpa_pair {
    struct wpa_ctrl command;
    struct wpa_ctrl monitor;
};

struct network_state {
    char interface[IFNAMSIZ];
    char state[24];
    char connectivity[24];
    char recovery_stage[24];
    char ssid[128];
    char ip[INET_ADDRSTRLEN];
    char gateway[INET_ADDRSTRLEN];
    char dns[INET_ADDRSTRLEN];
    char mac[18];
    int link_up;
    int signal;
    int rssi_dbm;
    int gateway_reachable;
    int liveness_failures;
};

struct client {
    int fd;
    char input[INPUT_MAX];
    size_t input_len;
    int busy;
};

struct pending_scan {
    int active;
    int client_fd;
    unsigned long id;
    long long deadline;
    long long poll_at;
};

struct pending_dhcp {
    int active;
    int release;
    pid_t pid;
    int client_fd;
    unsigned long id;
    long long deadline;
};

struct pending_association {
    int active;
    int client_fd;
    unsigned long id;
    int previous_network_id;
    int candidate_network_id;
    long long deadline;
    long long poll_at;
    char ssid[128];
};

struct daemon_ctx {
    int listen_fd;
    int netlink_fd;
    char socket_path[PATH_MAX];
    char wpa_path[PATH_MAX];
    char reboot_request_path[PATH_MAX];
    char reboot_guard_path[PATH_MAX];
    char interface[IFNAMSIZ];
    int foreground;
    int network_id;
    struct wpa_pair wpa;
    struct network_state state;
    struct le_network_health health;
    struct le_gateway_probe gateway_probe;
    struct client clients[CLIENT_MAX];
    struct pending_scan scan;
    struct pending_association association;
    struct pending_dhcp dhcp;
};

static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_child_event = 0;

static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void on_signal(int signo)
{
    if (signo == SIGTERM || signo == SIGINT)
        g_running = 0;
    else if (signo == SIGCHLD)
        g_child_event = 1;
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static void set_cloexec(int fd)
{
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static void copy_string(char *dst, size_t size, const char *src)
{
    size_t length;

    if (!size)
        return;
    if (!src)
        src = "";
    length = strlen(src);
    if (length >= size)
        length = size - 1;
    memcpy(dst, src, length);
    dst[length] = '\0';
}

static int append_text(char *dst, size_t size, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (*used >= size)
        return -1;
    va_start(ap, fmt);
    n = vsnprintf(dst + *used, size - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= size - *used)
        return -1;
    *used += (size_t)n;
    return 0;
}

static int append_json_string(char *dst, size_t size, size_t *used,
                             const char *value)
{
    const unsigned char *p = (const unsigned char *)(value ? value : "");
    if (append_text(dst, size, used, "\"") < 0)
        return -1;
    while (*p) {
        if (*p == '"' || *p == '\\') {
            if (append_text(dst, size, used, "\\%c", *p) < 0)
                return -1;
        } else if (*p == '\n') {
            if (append_text(dst, size, used, "\\n") < 0)
                return -1;
        } else if (*p == '\r') {
            if (append_text(dst, size, used, "\\r") < 0)
                return -1;
        } else if (*p == '\t') {
            if (append_text(dst, size, used, "\\t") < 0)
                return -1;
        } else if (*p >= 32) {
            if (append_text(dst, size, used, "%c", *p) < 0)
                return -1;
        }
        ++p;
    }
    return append_text(dst, size, used, "\"");
}

static int state_json(const struct network_state *s, char *out, size_t size)
{
    const char *gateway_reachable = s->gateway_reachable < 0 ? "null" :
                                    s->gateway_reachable ? "true" : "false";
    size_t n = 0;
    if (append_text(out, size, &n, "{\"interface\":") < 0 ||
        append_json_string(out, size, &n, s->interface) < 0 ||
        append_text(out, size, &n, ",\"state\":") < 0 ||
        append_json_string(out, size, &n, s->state) < 0 ||
        append_text(out, size, &n, ",\"connectivity\":") < 0 ||
        append_json_string(out, size, &n, s->connectivity) < 0 ||
        append_text(out, size, &n, ",\"recovery_stage\":") < 0 ||
        append_json_string(out, size, &n, s->recovery_stage) < 0 ||
        append_text(out, size, &n, ",\"ssid\":") < 0 ||
        append_json_string(out, size, &n, s->ssid) < 0 ||
        append_text(out, size, &n, ",\"ip\":") < 0 ||
        append_json_string(out, size, &n, s->ip) < 0 ||
        append_text(out, size, &n, ",\"gateway\":") < 0 ||
        append_json_string(out, size, &n, s->gateway) < 0 ||
        append_text(out, size, &n, ",\"dns\":") < 0 ||
        append_json_string(out, size, &n, s->dns) < 0 ||
        append_text(out, size, &n,
                    ",\"signal\":%d,\"rssi_dbm\":%d,\"gateway_reachable\":%s,\"liveness_failures\":%d,\"mac\":",
                    s->signal, s->rssi_dbm, gateway_reachable,
                    s->liveness_failures) < 0 ||
        append_json_string(out, size, &n, s->mac) < 0 ||
        append_text(out, size, &n, "}") < 0)
        return -1;
    return (int)n;
}

static int state_equal(const struct network_state *a,
                       const struct network_state *b)
{
    return a->link_up == b->link_up && a->signal == b->signal &&
           a->rssi_dbm == b->rssi_dbm &&
           a->gateway_reachable == b->gateway_reachable &&
           a->liveness_failures == b->liveness_failures &&
           !strcmp(a->interface, b->interface) && !strcmp(a->state, b->state) &&
           !strcmp(a->connectivity, b->connectivity) &&
           !strcmp(a->recovery_stage, b->recovery_stage) &&
           !strcmp(a->ssid, b->ssid) && !strcmp(a->ip, b->ip) &&
           !strcmp(a->gateway, b->gateway) && !strcmp(a->dns, b->dns) &&
           !strcmp(a->mac, b->mac);
}

/* ----- Direct wpa_supplicant control protocol -------------------------- */

static void wpa_ctrl_init(struct wpa_ctrl *ctrl)
{
    ctrl->fd = -1;
    ctrl->local_path[0] = '\0';
}

static void wpa_ctrl_close(struct wpa_ctrl *ctrl)
{
    if (ctrl->fd >= 0)
        close(ctrl->fd);
    if (ctrl->local_path[0])
        unlink(ctrl->local_path);
    wpa_ctrl_init(ctrl);
}

static int wpa_ctrl_open_one(struct wpa_ctrl *ctrl, const char *remote,
                             int serial)
{
    struct sockaddr_un local;
    struct sockaddr_un server;
    static unsigned int sequence;
    int fd;

    wpa_ctrl_init(ctrl);
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    set_cloexec(fd);

    memset(&local, 0, sizeof(local));
    local.sun_family = AF_UNIX;
    if (snprintf(local.sun_path, sizeof(local.sun_path),
                 "/tmp/libreecho-wpa-%ld-%u-%d", (long)getpid(),
                 sequence++, serial) >= (int)sizeof(local.sun_path)) {
        close(fd);
        return -1;
    }
    copy_string(ctrl->local_path, sizeof(ctrl->local_path), local.sun_path);
    unlink(local.sun_path);
    if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        close(fd);
        ctrl->local_path[0] = '\0';
        return -1;
    }

    memset(&server, 0, sizeof(server));
    server.sun_family = AF_UNIX;
    if (strlen(remote) >= sizeof(server.sun_path)) {
        close(fd);
        unlink(local.sun_path);
        ctrl->local_path[0] = '\0';
        return -1;
    }
    copy_string(server.sun_path, sizeof(server.sun_path), remote);
    if (connect(fd, (struct sockaddr *)&server, sizeof(server)) < 0) {
        close(fd);
        unlink(local.sun_path);
        ctrl->local_path[0] = '\0';
        return -1;
    }
    if (set_nonblock(fd) < 0) {
        close(fd);
        unlink(local.sun_path);
        ctrl->local_path[0] = '\0';
        return -1;
    }
    ctrl->fd = fd;
    return 0;
}

static int wpa_ctrl_recv(struct wpa_ctrl *ctrl, char *reply, size_t size)
{
    ssize_t n;
    if (ctrl->fd < 0 || !size) {
        errno = EBADF;
        return -1;
    }
    n = recv(ctrl->fd, reply, size - 1, MSG_DONTWAIT);
    if (n < 0)
        return -1;
    reply[n] = '\0';
    return (int)n;
}

static int wpa_ctrl_request(struct wpa_ctrl *ctrl, const char *command,
                            char *reply, size_t size, int timeout_ms)
{
    struct pollfd pfd;
    long long deadline = monotonic_ms() + timeout_ms;
    ssize_t sent;
    size_t command_length;

    if (ctrl->fd < 0 || !command || !reply || size < 2) {
        errno = EINVAL;
        return -1;
    }
    command_length = strlen(command);
    while (command_length && command[command_length - 1] == '\n')
        --command_length;
    sent = send(ctrl->fd, command, command_length, MSG_NOSIGNAL);
    if (sent < 0 || (size_t)sent != command_length)
        return -1;

    for (;;) {
        int remaining = (int)(deadline - monotonic_ms());
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        pfd.fd = ctrl->fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, remaining) < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = ECONNRESET;
            return -1;
        }
        if (pfd.revents & POLLIN) {
            int n = wpa_ctrl_recv(ctrl, reply, size);
            if (n >= 0)
                return n;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                return -1;
        }
    }
}

static int wpa_ctrl_attach(struct wpa_ctrl *ctrl)
{
    char reply[64];
    int n = wpa_ctrl_request(ctrl, "ATTACH\n", reply, sizeof(reply), WPA_TIMEOUT_MS);
    return n >= 3 && !strncmp(reply, "OK", 2) ? 0 : -1;
}

static void wpa_close(struct daemon_ctx *ctx)
{
    wpa_ctrl_close(&ctx->wpa.command);
    wpa_ctrl_close(&ctx->wpa.monitor);
}

static int wpa_open(struct daemon_ctx *ctx)
{
    if (ctx->wpa.command.fd >= 0)
        return 0;
    wpa_close(ctx);
    if (wpa_ctrl_open_one(&ctx->wpa.command, ctx->wpa_path, 0) < 0) {
        wpa_close(ctx);
        copy_string(ctx->state.state, sizeof(ctx->state.state), "unavailable");
        return -1;
    }
    /* Some vendor supplicants expose commands but reject ATTACH.  Keep the
     * command channel for status/scan operations and use result polling when
     * the optional event channel cannot be established. */
    if (wpa_ctrl_open_one(&ctx->wpa.monitor, ctx->wpa_path, 1) < 0 ||
        wpa_ctrl_attach(&ctx->wpa.monitor) < 0) {
        le_log_warn("networkd: wpa monitor unavailable; using result polling");
        wpa_ctrl_close(&ctx->wpa.monitor);
    }
    return 0;
}

static int wpa_call(struct daemon_ctx *ctx, const char *command,
                    char *reply, size_t size)
{
    int n;
    if (wpa_open(ctx) < 0)
        return -1;
    n = wpa_ctrl_request(&ctx->wpa.command, command, reply, size,
                         WPA_TIMEOUT_MS);
    if (n < 0) {
        wpa_close(ctx);
        copy_string(ctx->state.state, sizeof(ctx->state.state), "unavailable");
        return -1;
    }
    return n;
}

static int wpa_ok(struct daemon_ctx *ctx, const char *command,
                  char *reply, size_t size)
{
    int n = wpa_call(ctx, command, reply, size);
    return n >= 2 && !strncmp(reply, "OK", 2) ? 0 : -1;
}

/* A vendor supplicant can reject SCAN with FAIL-BUSY while a background
 * scan is already completing.  Treat that response as an in-progress scan
 * so the monitor event can deliver the resulting table to the caller. */
static int wpa_scan_request(struct daemon_ctx *ctx, char *reply, size_t size)
{
    int n = wpa_call(ctx, "SCAN\n", reply, size);
    if (n < 0)
        return -1;
    if (n >= 2 && !strncmp(reply, "OK", 2))
        return 0;
    if (n >= 9 && !strncmp(reply, "FAIL-BUSY", 9))
        return 1;
    if (n >= 15 && !strncmp(reply, "UNKNOWN COMMAND", 15))
        return 2;
    le_log_warn("networkd: wpa_supplicant rejected scan: %.80s", reply);
    return -1;
}

static const char *wpa_value(const char *status, const char *key,
                             char *value, size_t size)
{
    const char *p = status;
    size_t key_len = strlen(key);
    while (*p) {
        const char *end = strchr(p, '\n');
        size_t line_len = end ? (size_t)(end - p) : strlen(p);
        if (line_len > key_len && !strncmp(p, key, key_len) &&
            p[key_len] == '=') {
            size_t n = line_len - key_len - 1;
            if (n >= size)
                n = size - 1;
            memcpy(value, p + key_len + 1, n);
            value[n] = '\0';
            return value;
        }
        if (!end)
            break;
        p = end + 1;
    }
    if (size)
        value[0] = '\0';
    return NULL;
}

static int normalize_rssi_dbm(int rssi_dbm)
{
    if (rssi_dbm > 127 && rssi_dbm <= 255)
        return rssi_dbm - 256;
    return rssi_dbm;
}

static int rssi_to_percent(int rssi_dbm)
{
    int signal;

    if (rssi_dbm <= -100)
        return 0;
    if (rssi_dbm >= -30)
        return 100;
    signal = (rssi_dbm + 100) * 100 / 70;
    return signal < 0 ? 0 : signal > 100 ? 100 : signal;
}

/* Some vendor wpa_supplicant builds expose a control socket but reject the
 * direct monitor attach used by this small daemon.  The MTK WEXT driver still
 * provides the current dBm level through SIOCGIWSTATS, so retain useful live
 * telemetry instead of reporting an entirely unavailable network. */
static int read_wireless_rssi(const char *iface)
{
    struct iwreq request;
    struct iw_statistics statistics;
    int fd;
    int level;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    memset(&request, 0, sizeof(request));
    memset(&statistics, 0, sizeof(statistics));
    {
        size_t iface_len = strlen(iface);
        if (iface_len >= sizeof(request.ifr_name))
            iface_len = sizeof(request.ifr_name) - 1;
        memcpy(request.ifr_name, iface, iface_len);
        request.ifr_name[iface_len] = '\0';
    }
    request.u.data.pointer = &statistics;
    request.u.data.length = sizeof(statistics);
    request.u.data.flags = 1;
    if (ioctl(fd, SIOCGIWSTATS, &request) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    level = normalize_rssi_dbm((int)statistics.qual.level);
    return level <= 0 ? level : -1;
}

static int read_wireless_essid(const char *iface, char *out, size_t out_size)
{
    struct iwreq request;
    char essid[IW_ESSID_MAX_SIZE + 1];
    int fd;
    size_t length;

    if (!out_size)
        return -1;
    out[0] = '\0';
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    memset(&request, 0, sizeof(request));
    memset(essid, 0, sizeof(essid));
    {
        size_t iface_len = strlen(iface);
        if (iface_len >= sizeof(request.ifr_name))
            iface_len = sizeof(request.ifr_name) - 1;
        memcpy(request.ifr_name, iface, iface_len);
        request.ifr_name[iface_len] = '\0';
    }
    request.u.essid.pointer = essid;
    request.u.essid.length = IW_ESSID_MAX_SIZE;
    request.u.essid.flags = 0;
    if (ioctl(fd, SIOCGIWESSID, &request) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    length = request.u.essid.length;
    if (length > IW_ESSID_MAX_SIZE)
        length = IW_ESSID_MAX_SIZE;
    if (length >= out_size)
        length = out_size - 1;
    memcpy(out, essid, length);
    out[length] = '\0';
    return out[0] ? 0 : -1;
}

static void refresh_link_fallback(struct daemon_ctx *ctx)
{
    int rssi = read_wireless_rssi(ctx->interface);

    if (!ctx->state.ssid[0])
        (void)read_wireless_essid(ctx->interface, ctx->state.ssid,
                                  sizeof(ctx->state.ssid));
    copy_string(ctx->state.state, sizeof(ctx->state.state),
                ctx->state.link_up ? "connected" : "disconnected");
    if (rssi > -128) {
        ctx->state.rssi_dbm = rssi;
        ctx->state.signal = rssi_to_percent(rssi);
    } else {
        ctx->state.rssi_dbm = -1;
        ctx->state.signal = 0;
    }
}

/* ----- Interface information ------------------------------------------- */

static int interface_link_up(const char *name)
{
    int fd, result = 0;
    struct ifreq ifr;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    memset(&ifr, 0, sizeof(ifr));
    copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), name);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) == 0)
        result = ((ifr.ifr_flags & IFF_UP) && (ifr.ifr_flags & IFF_RUNNING));
    close(fd);
    return result;
}

static void read_mac(struct network_state *s)
{
    int fd;
    struct ifreq ifr;
    unsigned char *m;
    s->mac[0] = '\0';
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    memset(&ifr, 0, sizeof(ifr));
    copy_string(ifr.ifr_name, sizeof(ifr.ifr_name), s->interface);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
        m = (unsigned char *)ifr.ifr_hwaddr.sa_data;
        (void)snprintf(s->mac, sizeof(s->mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                       m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    close(fd);
}

static void read_ip(struct network_state *s)
{
    struct ifaddrs *all, *ifa;
    s->ip[0] = '\0';
    if (getifaddrs(&all) < 0)
        return;
    for (ifa = all; ifa; ifa = ifa->ifa_next) {
        char text[INET_ADDRSTRLEN];
        if (!ifa->ifa_name || strcmp(ifa->ifa_name, s->interface) ||
            !ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if (inet_ntop(AF_INET,
                      &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr,
                      text, sizeof(text))) {
            copy_string(s->ip, sizeof(s->ip), text);
            break;
        }
    }
    freeifaddrs(all);
}

static void read_route(struct network_state *s)
{
    FILE *fp;
    char line[256], dev[IFNAMSIZ];
    unsigned long destination, gateway, flags;
    s->gateway[0] = '\0';
    fp = fopen("/proc/net/route", "r");
    if (!fp)
        return;
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return;
    }
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%15s %lx %lx %lx", dev, &destination, &gateway,
                   &flags) != 4)
            continue;
        if (strcmp(dev, s->interface) || destination != 0 || !(flags & 1))
            continue;
        {
            struct in_addr addr;
            char text[INET_ADDRSTRLEN];
            addr.s_addr = (uint32_t)gateway;
            if (inet_ntop(AF_INET, &addr, text, sizeof(text)))
                copy_string(s->gateway, sizeof(s->gateway), text);
        }
        break;
    }
    fclose(fp);
}

static void read_dns(struct network_state *s)
{
    FILE *fp;
    char line[256], address[INET_ADDRSTRLEN];
    s->dns[0] = '\0';
    fp = fopen("/etc/resolv.conf", "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, " nameserver %15s", address) == 1 ||
            sscanf(line, "nameserver %15s", address) == 1) {
            copy_string(s->dns, sizeof(s->dns), address);
            break;
        }
    }
    fclose(fp);
}

/* The fib_trie fallback is useful on old minimal images where getifaddrs is
 * not populated until after the first address notification. */
static void read_ip_fib_trie(struct network_state *s)
{
    FILE *fp;
    char line[256], candidate[INET_ADDRSTRLEN];
    int in_iface = 0;
    if (s->ip[0])
        return;
    fp = fopen("/proc/net/fib_trie", "r");
    if (!fp)
        return;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "|-- ")) {
            char *p = strstr(line, "|-- ");
            p += 5;
            if (sscanf(p, "%15s", candidate) == 1 &&
                inet_pton(AF_INET, candidate, &(struct in_addr){0}) == 1)
                in_iface = 1;
            else
                in_iface = 0;
        } else if (in_iface && strstr(line, "host LOCAL")) {
            char *p = strstr(line, "32 host LOCAL");
            if (p) {
                p = line;
                while (*p == ' ' || *p == '\t')
                    ++p;
                if (sscanf(p, "%15s", candidate) == 1) {
                    struct in_addr addr;
                    if (inet_pton(AF_INET, candidate, &addr) == 1 &&
                        strcmp(candidate, "127.0.0.1")) {
                        copy_string(s->ip, sizeof(s->ip), candidate);
                        break;
                    }
                }
            }
        }
    }
    fclose(fp);
}

static void refresh_interface_info(struct daemon_ctx *ctx)
{
    ctx->state.link_up = interface_link_up(ctx->interface);
    read_mac(&ctx->state);
    read_ip(&ctx->state);
    read_ip_fib_trie(&ctx->state);
    read_route(&ctx->state);
    read_dns(&ctx->state);
#ifdef LE_NETWORKD_TESTING
    if (getenv("LIBREECHO_NETWORKD_TEST_FIXTURE")) {
        ctx->state.link_up = 1;
        copy_string(ctx->state.ip, sizeof(ctx->state.ip), "192.0.2.10");
        copy_string(ctx->state.gateway, sizeof(ctx->state.gateway), "192.0.2.1");
        copy_string(ctx->state.dns, sizeof(ctx->state.dns), "192.0.2.53");
    }
#endif
}

static void refresh_wpa_info(struct daemon_ctx *ctx)
{
    char reply[WPA_REPLY_MAX], value[256];
    int n;

    if (ctx->wpa.command.fd < 0) {
        refresh_link_fallback(ctx);
        return;
    }
    n = wpa_ctrl_request(&ctx->wpa.command, "STATUS\n", reply, sizeof(reply),
                         WPA_TIMEOUT_MS);
    if (n < 0) {
        wpa_close(ctx);
        refresh_link_fallback(ctx);
        return;
    }
    if (wpa_value(reply, "ssid", value, sizeof(value)))
        copy_string(ctx->state.ssid, sizeof(ctx->state.ssid), value);
    if (!ctx->state.ssid[0])
        (void)read_wireless_essid(ctx->interface, ctx->state.ssid,
                                  sizeof(ctx->state.ssid));
    if (wpa_value(reply, "wpa_state", value, sizeof(value))) {
        if (!strcmp(value, "COMPLETED"))
            copy_string(ctx->state.state, sizeof(ctx->state.state), "connected");
        else if (!strcmp(value, "ASSOCIATING") || !strcmp(value, "ASSOCIATED") ||
                 !strcmp(value, "4WAY_HANDSHAKE") ||
                 !strcmp(value, "GROUP_HANDSHAKE"))
            copy_string(ctx->state.state, sizeof(ctx->state.state), "connecting");
        else if (!strcmp(value, "DISCONNECTED"))
            copy_string(ctx->state.state, sizeof(ctx->state.state), "disconnected");
        else
            copy_string(ctx->state.state, sizeof(ctx->state.state), "connecting");
    }
    n = wpa_ctrl_request(&ctx->wpa.command, "SIGNAL_POLL\n", reply,
                         sizeof(reply), WPA_TIMEOUT_MS);
    if (n >= 0 && wpa_value(reply, "RSSI", value, sizeof(value))) {
        ctx->state.rssi_dbm = normalize_rssi_dbm((int)strtol(value, NULL, 10));
        ctx->state.signal = rssi_to_percent(ctx->state.rssi_dbm);
    } else {
        ctx->state.rssi_dbm = -1;
        ctx->state.signal = 0;
    }
}

static void sync_health_state(struct daemon_ctx *ctx)
{
    copy_string(ctx->state.connectivity, sizeof(ctx->state.connectivity),
                le_network_health_connectivity(&ctx->health));
    copy_string(ctx->state.recovery_stage, sizeof(ctx->state.recovery_stage),
                le_network_health_recovery_stage(&ctx->health));
    ctx->state.gateway_reachable =
        le_network_health_gateway_reachable(&ctx->health);
    ctx->state.liveness_failures =
        le_network_health_consecutive_failures(&ctx->health);
}

static void reset_network_health(struct daemon_ctx *ctx, long long now_ms)
{
    le_gateway_probe_close(&ctx->gateway_probe);
    le_network_health_init(&ctx->health, NULL, now_ms);
    sync_health_state(ctx);
}

static void refresh_state(struct daemon_ctx *ctx)
{
    refresh_interface_info(ctx);
    refresh_wpa_info(ctx);
    sync_health_state(ctx);
}

static int ensure_wpa(struct daemon_ctx *ctx)
{
    if (wpa_open(ctx) < 0)
        return -1;
    refresh_wpa_info(ctx);
    return 0;
}

/* ----- Adapter clients -------------------------------------------------- */

static int client_index(const struct daemon_ctx *ctx, int fd)
{
    int i;
    for (i = 0; i < CLIENT_MAX; ++i)
        if (ctx->clients[i].fd == fd)
            return i;
    return -1;
}

static int send_bytes(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EINTR))
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd;
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, 1000) > 0)
                continue;
        }
        return -1;
    }
    return 0;
}

static int send_ok_fd(int fd, unsigned long id, const char *data)
{
    char response[LE_ADAPTER_MSG_MAX];
    int n = le_adapter_respond_ok(response, sizeof(response), id,
                                   data ? data : "{}");
    if (n < 0)
        return -1;
    return send_bytes(fd, response, (size_t)n);
}

static int send_err_fd(int fd, unsigned long id, const char *error)
{
    char response[LE_ADAPTER_MSG_MAX];
    int n = le_adapter_respond_err(response, sizeof(response), id,
                                    error ? error : "network error");
    if (n < 0)
        return -1;
    return send_bytes(fd, response, (size_t)n);
}

static void remove_client(struct daemon_ctx *ctx, int index)
{
    int fd;
    if (index < 0 || index >= CLIENT_MAX || ctx->clients[index].fd < 0)
        return;
    fd = ctx->clients[index].fd;
    if (ctx->scan.active && ctx->scan.client_fd == fd)
        ctx->scan.client_fd = -1;
    if (ctx->association.active && ctx->association.client_fd == fd)
        ctx->association.client_fd = -1;
    if (ctx->dhcp.active && ctx->dhcp.client_fd == fd)
        ctx->dhcp.client_fd = -1;
    close(fd);
    ctx->clients[index].fd = -1;
    ctx->clients[index].input_len = 0;
    ctx->clients[index].busy = 0;
}

static void broadcast_state(struct daemon_ctx *ctx, const char *event_type)
{
    char data[LE_ADAPTER_MSG_MAX], event[LE_ADAPTER_MSG_MAX];
    int i, n;
    if (state_json(&ctx->state, data, sizeof(data)) < 0)
        return;
    n = le_adapter_format_event(event, sizeof(event), event_type, data);
    if (n < 0)
        return;
    for (i = 0; i < CLIENT_MAX; ++i) {
        if (ctx->clients[i].fd >= 0 &&
            send_bytes(ctx->clients[i].fd, event, (size_t)n) < 0)
            remove_client(ctx, i);
    }
}

static void refresh_and_broadcast(struct daemon_ctx *ctx, const char *event_type)
{
    struct network_state before = ctx->state;
    refresh_state(ctx);
    if (!state_equal(&before, &ctx->state))
        broadcast_state(ctx, event_type);
}

#ifdef LE_NETWORKD_TESTING
static void test_log_health_action(const char *action)
{
    const char *path = getenv("LIBREECHO_NETWORKD_TEST_ACTION_LOG");
    int fd;

    if (!path || !path[0] || !action)
        return;
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    (void)write(fd, action, strlen(action));
    (void)write(fd, "\n", 1);
    (void)close(fd);
}
#endif

static int set_interface_enabled(const char *interface, int enabled)
{
    struct ifreq request;
    int fd;
    int result;
#ifdef LE_NETWORKD_TESTING
    if (getenv("LIBREECHO_NETWORKD_TEST_FIXTURE")) {
        const char *failure = getenv("LIBREECHO_NETWORKD_TEST_FAIL_INTERFACE");
        const char *action = enabled ? "up" : "down";
        if (failure && (!strcmp(failure, "all") || !strcmp(failure, action))) {
            errno = EIO;
            return -1;
        }
        return 0;
    }
#endif

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    memset(&request, 0, sizeof(request));
    copy_string(request.ifr_name, sizeof(request.ifr_name), interface);
    result = ioctl(fd, SIOCGIFFLAGS, &request);
    if (result == 0) {
        if (enabled)
            request.ifr_flags = (short)(request.ifr_flags | IFF_UP);
        else
            request.ifr_flags = (short)(request.ifr_flags & ~IFF_UP);
        result = ioctl(fd, SIOCSIFFLAGS, &request);
    }
    close(fd);
    return result;
}

static int file_contains_reboot_request(const char *path)
{
    static const char request[] = "reboot\n";
    char contents[sizeof(request)];
    struct stat status;
    ssize_t length;
    int fd;

    fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return -1;
    if (fstat(fd, &status) < 0) {
        int saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
    if (!S_ISREG(status.st_mode)) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }
    length = read(fd, contents, sizeof(contents));
    if (close(fd) < 0 && length >= 0)
        return -1;
    return length == (ssize_t)(sizeof(request) - 1) &&
           !memcmp(contents, request, sizeof(request) - 1);
}

static int reserve_reboot_guard(const char *guard_path)
{
    static const char marker[] = "network-reboot-v1\n";
    int fd;
    ssize_t written;

    fd = open(guard_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                          O_NOFOLLOW, 0600);
    if (fd < 0)
        return errno == EEXIST ? 1 : -1;
    written = write(fd, marker, sizeof(marker) - 1);
    if (written != (ssize_t)(sizeof(marker) - 1)) {
        int saved_errno = written < 0 ? errno : EIO;
        (void)close(fd);
        (void)unlink(guard_path);
        errno = saved_errno;
        return -1;
    }
    if (fsync(fd) < 0) {
        int saved_errno = errno;
        (void)close(fd);
        (void)unlink(guard_path);
        errno = saved_errno;
        return -1;
    }
    if (close(fd) < 0) {
        int saved_errno = errno;
        (void)unlink(guard_path);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static int request_supervised_reboot(const char *request_path)
{
    char temporary[PATH_MAX];
    static const char request[] = "reboot\n";
    int fd;
    ssize_t written;

    if (!request_path || !request_path[0] ||
        snprintf(temporary, sizeof(temporary), "%s.networkd", request_path) >=
            (int)sizeof(temporary)) {
        errno = EINVAL;
        return -1;
    }
    (void)unlink(temporary);
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                         O_NOFOLLOW, 0600);
    if (fd < 0)
        return -1;
    written = write(fd, request, sizeof(request) - 1);
    if (written != (ssize_t)(sizeof(request) - 1)) {
        int saved_errno = written < 0 ? errno : EIO;
        (void)close(fd);
        (void)unlink(temporary);
        errno = saved_errno;
        return -1;
    }
    if (fsync(fd) < 0) {
        int saved_errno = errno;
        (void)close(fd);
        (void)unlink(temporary);
        errno = saved_errno;
        return -1;
    }
    if (close(fd) < 0) {
        int saved_errno = errno;
        (void)unlink(temporary);
        errno = saved_errno;
        return -1;
    }
    if (link(temporary, request_path) < 0) {
        int saved_errno = errno;
        (void)unlink(temporary);
        if (saved_errno == EEXIST && file_contains_reboot_request(request_path) == 1)
            return 0;
        errno = saved_errno;
        return -1;
    }
    (void)unlink(temporary);
    return 0;
}

static void perform_health_action(struct daemon_ctx *ctx,
                                  enum le_network_health_action action)
{
    char reply[WPA_REPLY_MAX];
    int result;
#ifdef LE_NETWORKD_TESTING
    if (action == LE_NETWORK_HEALTH_REASSOCIATE)
        test_log_health_action("reassociate");
    else if (action == LE_NETWORK_HEALTH_INTERFACE_DOWN)
        test_log_health_action("interface-down");
    else if (action == LE_NETWORK_HEALTH_INTERFACE_UP)
        test_log_health_action("interface-up");
    else if (action == LE_NETWORK_HEALTH_REQUEST_REBOOT)
        test_log_health_action("reboot-request");
#endif

    if (action == LE_NETWORK_HEALTH_REASSOCIATE) {
        result = wpa_ok(ctx, "REASSOCIATE\n", reply, sizeof(reply));
        if (result == 0)
            le_log_warn("networkd: gateway liveness failed; wpa reassociation requested");
        else
            le_log_error("networkd: gateway liveness failed; wpa reassociation request failed");
    } else if (action == LE_NETWORK_HEALTH_INTERFACE_DOWN) {
        result = set_interface_enabled(ctx->interface, 0);
        if (result == 0)
            le_log_warn("networkd: gateway still unreachable; interface reset started");
        else
            le_log_error("networkd: unable to bring interface down for recovery: %s",
                         strerror(errno));
    } else if (action == LE_NETWORK_HEALTH_INTERFACE_UP) {
        result = set_interface_enabled(ctx->interface, 1);
        if (result == 0) {
            le_log_warn("networkd: interface restored; waiting for reassociation");
            if (wpa_ok(ctx, "REASSOCIATE\n", reply, sizeof(reply)) < 0)
                le_log_warn("networkd: post-reset wpa reassociation request failed");
        } else {
            le_log_error("networkd: unable to restore interface after reset: %s",
                         strerror(errno));
        }
    } else if (action == LE_NETWORK_HEALTH_REQUEST_REBOOT) {
        int guard = reserve_reboot_guard(ctx->reboot_guard_path);
        int submitted = 0;

        if (guard == 0) {
            if (request_supervised_reboot(ctx->reboot_request_path) == 0) {
                submitted = 1;
                le_log_error("networkd: recovery exhausted; supervised reboot requested");
            } else {
                int saved_errno = errno;
                (void)unlink(ctx->reboot_guard_path);
                le_log_error("networkd: recovery exhausted; unable to request supervised reboot: %s",
                             strerror(saved_errno));
            }
        } else if (guard > 0) {
            le_log_error("networkd: recovery exhausted; persistent reboot budget already consumed");
        } else {
            le_log_error("networkd: recovery exhausted; unable to reserve persistent reboot budget: %s",
                         strerror(errno));
        }
        le_network_health_finish_reboot_request(&ctx->health, submitted);
    }
}

static void publish_health_change(struct daemon_ctx *ctx,
                                  const struct network_state *before)
{
    sync_health_state(ctx);
    if (!state_equal(before, &ctx->state))
        broadcast_state(ctx, "network.health");
}

static void record_gateway_probe(struct daemon_ctx *ctx,
                                 enum le_gateway_probe_result result,
                                 long long now_ms)
{
    struct network_state before = ctx->state;
    int prior_failures = le_network_health_consecutive_failures(&ctx->health);
    enum le_network_health_action action =
        le_network_health_record_probe(&ctx->health, now_ms, result);

    if (result == LE_GATEWAY_REACHABLE && prior_failures > 0)
        le_log_info("networkd: gateway liveness recovered after %d failed probe(s)",
                    prior_failures);
    else if (result == LE_GATEWAY_UNREACHABLE)
        le_log_warn("networkd: gateway liveness probe failed (%d consecutive)",
                    le_network_health_consecutive_failures(&ctx->health));
    perform_health_action(ctx, action);
    publish_health_change(ctx, &before);
}

static void handle_gateway_probe_event(struct daemon_ctx *ctx, short revents,
                                       long long now_ms)
{
    int result;

    if (!ctx->gateway_probe.active)
        return;
    if (revents & POLLIN) {
        result = le_gateway_probe_receive(&ctx->gateway_probe);
        if (result > 0) {
            le_gateway_probe_close(&ctx->gateway_probe);
            record_gateway_probe(ctx, LE_GATEWAY_REACHABLE, now_ms);
            return;
        }
        if (result < 0) {
            le_gateway_probe_close(&ctx->gateway_probe);
            record_gateway_probe(ctx, LE_GATEWAY_PROBE_UNAVAILABLE, now_ms);
            return;
        }
    }
    if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
        le_gateway_probe_close(&ctx->gateway_probe);
        record_gateway_probe(ctx, LE_GATEWAY_PROBE_UNAVAILABLE, now_ms);
    }
}

static void check_network_health(struct daemon_ctx *ctx, long long now_ms)
{
    struct network_state before = ctx->state;
    enum le_network_health_action action;
    int associated;

    associated = !strcmp(ctx->state.state, "connected") &&
                 ctx->state.ssid[0] && ctx->state.link_up;
    if (associated && (ctx->dhcp.active || ctx->scan.active ||
                       !ctx->state.ip[0])) {
        if (ctx->gateway_probe.active)
            le_gateway_probe_close(&ctx->gateway_probe);
        record_gateway_probe(ctx, LE_GATEWAY_PROBE_UNAVAILABLE, now_ms);
        return;
    }
    if (le_gateway_probe_timed_out(&ctx->gateway_probe, now_ms)) {
        le_gateway_probe_close(&ctx->gateway_probe);
        record_gateway_probe(ctx, LE_GATEWAY_UNREACHABLE, now_ms);
        return;
    }
    action = le_network_health_tick(&ctx->health, now_ms, associated,
                                    ctx->state.gateway[0] != '\0');
    if (action == LE_NETWORK_HEALTH_PROBE) {
        if (!ctx->state.gateway[0]) {
            record_gateway_probe(ctx, LE_GATEWAY_UNREACHABLE, now_ms);
        } else if (le_gateway_probe_start(&ctx->gateway_probe, ctx->interface,
                                          ctx->state.gateway, now_ms) < 0) {
            le_log_warn("networkd: gateway liveness probe unavailable: %s",
                        strerror(errno));
            record_gateway_probe(ctx, LE_GATEWAY_PROBE_UNAVAILABLE, now_ms);
        } else {
            publish_health_change(ctx, &before);
        }
        return;
    }
    perform_health_action(ctx, action);
    publish_health_change(ctx, &before);
}

/* ----- DHCP child handling --------------------------------------------- */

static void remove_network_profile(struct daemon_ctx *ctx, int id);
static void restore_previous_network(struct daemon_ctx *ctx);

static void finish_dhcp(struct daemon_ctx *ctx, int status, int timed_out)
{
    int fd = ctx->dhcp.client_fd;
    unsigned long id = ctx->dhcp.id;
    int release = ctx->dhcp.release;
    char data[LE_ADAPTER_MSG_MAX], command[64], reply[WPA_REPLY_MAX];
    int success = !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    int network_success;

    ctx->dhcp.active = 0;
    ctx->dhcp.pid = -1;
    if (release) {
        ctx->state.ip[0] = '\0';
        ctx->state.gateway[0] = '\0';
        ctx->state.dns[0] = '\0';
        copy_string(ctx->state.state, sizeof(ctx->state.state), "disconnected");
        network_success = success;
    } else {
        int previous = ctx->association.previous_network_id;
        int candidate = ctx->association.candidate_network_id;
        refresh_interface_info(ctx);
        network_success = success && ctx->state.ip[0];
        if (network_success && previous >= 0 && previous != candidate) {
            remove_network_profile(ctx, previous);
            if (wpa_ok(ctx, "SAVE_CONFIG\n", reply, sizeof(reply)) < 0) {
                (void)wpa_ok(ctx, "RECONFIGURE\n", reply, sizeof(reply));
                remove_network_profile(ctx, candidate);
                if (snprintf(command, sizeof(command), "SELECT_NETWORK %d\n",
                             previous) < (int)sizeof(command))
                    (void)wpa_ok(ctx, command, reply, sizeof(reply));
                (void)wpa_ok(ctx, "SAVE_CONFIG\n", reply, sizeof(reply));
                ctx->network_id = previous;
                network_success = 0;
            }
        } else if (!network_success) {
            restore_previous_network(ctx);
        }
        copy_string(ctx->state.state, sizeof(ctx->state.state),
                    network_success ? "connected" : "disconnected");
        memset(&ctx->association, 0, sizeof(ctx->association));
        ctx->association.client_fd = -1;
    }
    if (fd >= 0) {
        int ci = client_index(ctx, fd);
        if (ci >= 0) {
            if (network_success)
                (void)send_ok_fd(fd, id, state_json(&ctx->state, data, sizeof(data)) >= 0 ? data : "{}");
            else if (success && !release && !ctx->state.ip[0])
                (void)send_err_fd(fd, id, "DHCP completed without an IPv4 address");
            else if (timed_out)
                (void)send_err_fd(fd, id, "DHCP timed out");
            else
                (void)send_err_fd(fd, id, "DHCP failed");
            ctx->clients[ci].busy = 0;
        }
    }
    broadcast_state(ctx, "network_changed");
}

static int start_dhcp(struct daemon_ctx *ctx, int release, int client_fd,
                      unsigned long id)
{
    pid_t pid;
    if (ctx->dhcp.active)
        return -1;
#ifdef LE_NETWORKD_TESTING
    if (getenv("LIBREECHO_NETWORKD_TEST_FIXTURE")) {
        errno = EOPNOTSUPP;
        return -1;
    }
#endif
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (release)
            execl("/bin/udhcpc", "udhcpc", "-i", ctx->interface, "-n", "-q",
                  "-R", "-s", "/etc/udhcpc.script", (char *)NULL);
        else
            execl("/bin/udhcpc", "udhcpc", "-i", ctx->interface, "-n", "-q",
                  "-s", "/etc/udhcpc.script", (char *)NULL);
        _exit(127);
    }
    ctx->dhcp.active = 1;
    ctx->dhcp.release = release;
    ctx->dhcp.pid = pid;
    ctx->dhcp.client_fd = client_fd;
    ctx->dhcp.id = id;
    ctx->dhcp.deadline = monotonic_ms() + DHCP_TIMEOUT_MS;
    return 0;
}

static void reap_children(struct daemon_ctx *ctx)
{
    int status;
    pid_t pid;
    for (;;) {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;
        if (ctx->dhcp.active && pid == ctx->dhcp.pid)
            finish_dhcp(ctx, status, 0);
    }
    g_child_event = 0;
}

static void cancel_dhcp(struct daemon_ctx *ctx)
{
    int status;
    if (!ctx->dhcp.active)
        return;
    (void)kill(ctx->dhcp.pid, SIGTERM);
    if (waitpid(ctx->dhcp.pid, &status, 0) < 0)
        memset(&status, 0, sizeof(status));
    finish_dhcp(ctx, status, 1);
}

static void check_dhcp_timeout(struct daemon_ctx *ctx)
{
    int status;
    if (!ctx->dhcp.active || monotonic_ms() < ctx->dhcp.deadline)
        return;
    (void)kill(ctx->dhcp.pid, SIGKILL);
    if (waitpid(ctx->dhcp.pid, &status, 0) < 0)
        memset(&status, 0, sizeof(status));
    finish_dhcp(ctx, status, 1);
}

/* ----- Scan handling ---------------------------------------------------- */

static const char *scan_security(const char *flags)
{
    if (!flags || !*flags)
        return "open";
    if (strstr(flags, "SAE") || strstr(flags, "WPA3"))
        return "unsupported";
    if (strstr(flags, "WPA2"))
        return "wpa2";
    if (strstr(flags, "WPA"))
        return "wpa";
    return "open";
}

struct scan_result {
    char ssid[IW_ESSID_MAX_SIZE + 1];
    char flags[128];
    int signal;
    int signal_percent;
    int frequency;
    int five_ghz;
};

static int scan_result_strength(const struct scan_result *result)
{
    if (result->signal_percent >= 0)
        return result->signal_percent;
    return rssi_to_percent(result->signal);
}

static int scan_result_better(const struct scan_result *a,
                              const struct scan_result *b)
{
    int a_strength, b_strength;

    if (a->five_ghz != b->five_ghz)
        return a->five_ghz > b->five_ghz;
    a_strength = scan_result_strength(a);
    b_strength = scan_result_strength(b);
    if (a_strength != b_strength)
        return a_strength > b_strength;
    if (a->signal_percent < 0 && b->signal_percent < 0 &&
        a->signal != b->signal)
        return a->signal > b->signal;
    return strcmp(a->ssid, b->ssid) < 0;
}

static int serialize_scan_results(struct scan_result *results, int count,
                                  char *data, size_t size)
{
    size_t used = 0;
    int i, emitted = 0;

    for (i = 0; i < count; ++i) {
        int j;
        for (j = i + 1; j < count; ++j) {
            if (scan_result_better(&results[j], &results[i])) {
                struct scan_result swap = results[i];
                results[i] = results[j];
                results[j] = swap;
            }
        }
    }
    if (append_text(data, size, &used, "{\"networks\":[") < 0)
        return -1;
    for (i = 0; i < count; ++i) {
        char entry[640];
        size_t entry_used = 0;
        int j, duplicate = 0;
        for (j = 0; j < i; ++j)
            if (!strcmp(results[j].ssid, results[i].ssid))
                duplicate = 1;
        if (duplicate)
            continue;
        if (emitted >= SCAN_OUTPUT_MAX)
            break;
        if (append_text(entry, sizeof(entry), &entry_used, "{\"ssid\":") < 0 ||
            append_json_string(entry, sizeof(entry), &entry_used, results[i].ssid) < 0 ||
            append_text(entry, sizeof(entry), &entry_used, ",\"security\":") < 0 ||
            append_json_string(entry, sizeof(entry), &entry_used,
                               scan_security(results[i].flags)) < 0 ||
            append_text(entry, sizeof(entry), &entry_used, ",\"signal\":%d}",
                        results[i].signal_percent >= 0 ?
                        results[i].signal_percent :
                        rssi_to_percent(results[i].signal)) < 0)
            return -1;
        if (used + entry_used + (emitted ? 1 : 0) + 2 >= size)
            break;
        if (emitted++ && append_text(data, size, &used, ",") < 0)
            return -1;
        if (append_text(data, size, &used, "%s", entry) < 0)
            return -1;
    }
    if (append_text(data, size, &used, "]}") < 0)
        return -1;
    return (int)used;
}

static int wext_frequency_mhz(const struct iw_freq *frequency)
{
    long long value;
    int exponent;

    if (!frequency)
        return 0;
    value = frequency->m;
    exponent = frequency->e;
    while (exponent-- > 0 && value <= 6000000000LL)
        value *= 10;
    if (value > 1000000)
        value /= 1000000;
    return value > 0 && value < 10000 ? (int)value : 0;
}

static int bogus_ssid(const char *ssid);

static int parse_scan_results(const char *reply, char *data, size_t size)
{
    const char *line = reply;
    int count = 0, i;
    struct scan_result results[SCAN_MAX];
    memset(results, 0, sizeof(results));
    while (*line) {
        char row[512], *fields[5], *p;
        char ssid[256], flags[128], signal[32], frequency[32];
        int field = 0, duplicate = -1, level;
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len >= sizeof(row))
            len = sizeof(row) - 1;
        memcpy(row, line, len);
        row[len] = '\0';
        p = row;
        fields[field++] = p;
        while (*p && field < 5) {
            if (*p == '\t') {
                *p = '\0';
                fields[field++] = p + 1;
            }
            ++p;
        }
        if (field == 5 && strcmp(fields[0], "bssid / frequency / signal level / flags / ssid")) {
            copy_string(signal, sizeof(signal), fields[2]);
            copy_string(frequency, sizeof(frequency), fields[1]);
            copy_string(flags, sizeof(flags), fields[3]);
            copy_string(ssid, sizeof(ssid), fields[4]);
            if (bogus_ssid(ssid)) {
                if (!end)
                    break;
                line = end + 1;
                continue;
            }
            level = (int)strtol(signal, NULL, 10);
            {
                int freq = (int)strtol(frequency, NULL, 10);
                int five_ghz = freq >= 5000 && freq < 6000;
                for (i = 0; i < count; ++i) {
                    if (!strcmp(results[i].ssid, ssid)) {
                        duplicate = i;
                        break;
                    }
                }
                if (duplicate >= 0) {
                    if (five_ghz > results[duplicate].five_ghz ||
                        (five_ghz == results[duplicate].five_ghz &&
                         level > results[duplicate].signal)) {
                        copy_string(results[duplicate].flags,
                                    sizeof(results[duplicate].flags), flags);
                        results[duplicate].signal = level;
                        results[duplicate].signal_percent = -1;
                        results[duplicate].frequency = freq;
                        results[duplicate].five_ghz = five_ghz;
                    }
                } else if (count < SCAN_MAX) {
                    copy_string(results[count].ssid,
                                sizeof(results[count].ssid), ssid);
                    copy_string(results[count].flags,
                                sizeof(results[count].flags), flags);
                    results[count].signal = level;
                    results[count].signal_percent = -1;
                    results[count].frequency = freq;
                    results[count++].five_ghz = five_ghz;
                }
            }
        }
        if (!end)
            break;
        line = end + 1;
    }
    for (i = 0; i < count; ++i) {
        int j;
        for (j = i + 1; j < count; ++j) {
            if (scan_result_better(&results[j], &results[i])) {
                struct scan_result swap = results[i];
                results[i] = results[j];
                results[j] = swap;
            }
        }
    }
    return serialize_scan_results(results, count, data, size);
}

struct wext_scan_cell {
    char ssid[IW_ESSID_MAX_SIZE + 1];
    char security[16];
    int signal;
    int encrypted;
    int frequency;
    int rssi_dbm;
    int five_ghz;
};

static int bogus_ssid(const char *ssid)
{
    size_t i;
    if (!ssid || !*ssid || !strncmp(ssid, "NVRAM WARNING:", 14))
        return 1;
    if (!strncmp(ssid, "\\x00", 4)) {
        for (i = 0; ssid[i]; ++i)
            if (ssid[i] != '\\' && ssid[i] != 'x' &&
                (ssid[i] < '0' || ssid[i] > '9') &&
                (ssid[i] < 'a' || ssid[i] > 'f') &&
                (ssid[i] < 'A' || ssid[i] > 'F'))
                return 0;
        return 1;
    }
    for (i = 0; ssid[i]; ++i)
        if ((unsigned char)ssid[i] < 32)
            return 1;
    return 0;
}

static int wext_signal_percent(const struct iw_quality *quality)
{
    int level;

    if (!quality)
        return 0;
    if (quality->updated & IW_QUAL_DBM) {
        level = (int)(signed char)quality->level;
        return rssi_to_percent(level);
    }
    return quality->qual > 100 ? 0 : quality->qual;
}

static struct wext_scan_cell *wext_new_cell(struct wext_scan_cell *cells,
                                             size_t *count)
{
    struct wext_scan_cell *cell;

    if (!cells || !count || *count >= SCAN_MAX)
        return NULL;
    cell = &cells[(*count)++];
    memset(cell, 0, sizeof(*cell));
    copy_string(cell->security, sizeof(cell->security), "unknown");
    cell->rssi_dbm = -127;
    return cell;
}

static void wext_parse_ies(struct wext_scan_cell *cell,
                           const unsigned char *payload, size_t length)
{
    size_t offset = 0;

    if (!cell || !payload)
        return;
    while (offset + 2 <= length) {
        size_t ie_length = payload[offset + 1];
        if (offset + 2 + ie_length > length)
            break;
        if (payload[offset] == 0x30 ||
            (payload[offset] == 0xdd && ie_length >= 4 &&
             payload[offset + 2] == 0x00 &&
             payload[offset + 3] == 0x50 &&
             payload[offset + 4] == 0xf2 &&
             payload[offset + 5] == 0x01))
            cell->encrypted = 1;
        offset += 2 + ie_length;
    }
}

static int wext_parse_scan_events(const unsigned char *stream, size_t length,
                                  char *data, size_t data_size)
{
    struct wext_scan_cell cells[SCAN_MAX];
    struct wext_scan_cell *current = NULL;
    size_t offset = 0, count = 0, i;
    struct scan_result results[SCAN_MAX];
    int result_count = 0;

    memset(cells, 0, sizeof(cells));
    while (offset + IW_EV_LCP_PK_LEN <= length) {
        uint16_t event_length, command;
        const unsigned char *event = stream + offset;

        memcpy(&event_length, event, sizeof(event_length));
        memcpy(&command, event + 2, sizeof(command));
        if (event_length < IW_EV_LCP_PK_LEN ||
            offset + event_length > length)
            return -1;

        if (command == SIOCGIWAP) {
            current = wext_new_cell(cells, &count);
        } else if (current && command == SIOCGIWESSID &&
                   event_length >= IW_EV_POINT_PK_LEN) {
            uint16_t essid_length;
            size_t payload_length = event_length - IW_EV_POINT_PK_LEN;
            memcpy(&essid_length, event + IW_EV_LCP_PK_LEN,
                   sizeof(essid_length));
            if (essid_length < payload_length)
                payload_length = essid_length;
            if (payload_length >= sizeof(current->ssid))
                payload_length = sizeof(current->ssid) - 1;
            if (payload_length)
                memcpy(current->ssid, event + IW_EV_POINT_PK_LEN,
                       payload_length);
            current->ssid[payload_length] = '\0';
        } else if (current && command == SIOCGIWFREQ &&
                   event_length >= IW_EV_LCP_PK_LEN + sizeof(struct iw_freq)) {
            struct iw_freq frequency;
            memcpy(&frequency, event + IW_EV_LCP_PK_LEN, sizeof(frequency));
            current->frequency = wext_frequency_mhz(&frequency);
            current->five_ghz = current->frequency >= 5000 &&
                                current->frequency < 6000;
        } else if (current && command == IWEVQUAL &&
                   event_length >= IW_EV_QUAL_PK_LEN) {
            struct iw_quality quality;
            memcpy(&quality, event + IW_EV_LCP_PK_LEN, sizeof(quality));
            if (quality.updated & IW_QUAL_DBM)
                current->rssi_dbm = (int)(signed char)quality.level;
            current->signal = wext_signal_percent(&quality);
        } else if (current && command == SIOCGIWENCODE &&
                   event_length >= IW_EV_POINT_PK_LEN) {
            uint16_t flags;
            memcpy(&flags, event + IW_EV_LCP_PK_LEN + sizeof(uint16_t),
                   sizeof(flags));
            if (!(flags & IW_ENCODE_DISABLED))
                current->encrypted = 1;
        } else if (current && command == IWEVGENIE &&
                   event_length >= IW_EV_POINT_PK_LEN) {
            uint16_t ie_length;
            size_t payload_length = event_length - IW_EV_POINT_PK_LEN;
            memcpy(&ie_length, event + IW_EV_LCP_PK_LEN,
                   sizeof(ie_length));
            if (ie_length < payload_length)
                payload_length = ie_length;
            wext_parse_ies(current, event + IW_EV_POINT_PK_LEN,
                           payload_length);
        }
        offset += event_length;
    }

    if (offset != length)
        return -1;
    for (i = 0; i < count; ++i) {
        size_t j;
        for (j = i + 1; j < count; ++j) {
            int better = 0;
            if (cells[j].five_ghz != cells[i].five_ghz)
                better = cells[j].five_ghz > cells[i].five_ghz;
            else if (cells[j].rssi_dbm != cells[i].rssi_dbm)
                better = cells[j].rssi_dbm > cells[i].rssi_dbm;
            else if (cells[j].signal != cells[i].signal)
                better = cells[j].signal > cells[i].signal;
            else
                better = strcmp(cells[j].ssid, cells[i].ssid) < 0;
            if (better) {
                struct wext_scan_cell swap = cells[i];
                cells[i] = cells[j];
                cells[j] = swap;
            }
        }
    }
    memset(results, 0, sizeof(results));
    for (i = 0; i < count && result_count < SCAN_MAX; ++i) {
        size_t j;
        if (bogus_ssid(cells[i].ssid))
            continue;
        for (j = 0; j < (size_t)result_count; ++j)
            if (!strcmp(results[j].ssid, cells[i].ssid))
                break;
        if (j < (size_t)result_count)
            continue;
        copy_string(results[result_count].ssid,
                    sizeof(results[result_count].ssid), cells[i].ssid);
        copy_string(results[result_count].flags,
                    sizeof(results[result_count].flags),
                    cells[i].encrypted ? "WPA2" : "");
        results[result_count].signal = cells[i].rssi_dbm != -127 ?
                                       cells[i].rssi_dbm : cells[i].signal;
        results[result_count].signal_percent = cells[i].rssi_dbm == -127 ?
                                               cells[i].signal : -1;
        results[result_count].frequency = cells[i].frequency;
        results[result_count].five_ghz = cells[i].five_ghz;
        ++result_count;
    }
    return serialize_scan_results(results, result_count, data, data_size);
}

static int wext_scan(const char *iface, char *data, size_t data_size)
{
    struct iwreq request;
    unsigned char *buffer;
    long long deadline;
    int fd, scan_errno = 0;

    if (!iface || !data || data_size < 16)
        return -1;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    buffer = malloc(WEXT_SCAN_BUFFER_SIZE);
    if (!buffer) {
        close(fd);
        return -1;
    }

    memset(&request, 0, sizeof(request));
    copy_string(request.ifr_name, sizeof(request.ifr_name), iface);
    request.u.data.pointer = NULL;
    request.u.data.length = 0;
    request.u.data.flags = 0;
    if (ioctl(fd, SIOCSIWSCAN, &request) < 0 &&
        errno != EBUSY && errno != EINPROGRESS && errno != EPERM) {
        scan_errno = errno;
        goto failed;
    }

    deadline = monotonic_ms() + SCAN_TIMEOUT_MS;
    for (;;) {
        int result;
        memset(&request, 0, sizeof(request));
        copy_string(request.ifr_name, sizeof(request.ifr_name), iface);
        request.u.data.pointer = buffer;
        request.u.data.length = WEXT_SCAN_BUFFER_SIZE;
        request.u.data.flags = 0;
        result = ioctl(fd, SIOCGIWSCAN, &request);
        if (result == 0) {
            int parsed = wext_parse_scan_events(buffer, request.u.data.length,
                                                data, data_size);
            free(buffer);
            close(fd);
            return parsed;
        }
        if (errno != EAGAIN && errno != EBUSY && errno != EINPROGRESS) {
            scan_errno = errno;
            goto failed;
        }
        if (monotonic_ms() >= deadline) {
            scan_errno = ETIMEDOUT;
            goto failed;
        }
        (void)poll(NULL, 0, WEXT_SCAN_RETRY_MS);
    }

failed:
    le_log_warn("networkd: WEXT scan failed: %s", strerror(scan_errno));
    free(buffer);
    close(fd);
    return scan_errno ? -scan_errno : -EIO;
}

/* The MT8163 6.1 driver exposes cfg80211 but returns EOPNOTSUPP from the
 * compatibility WEXT scan ioctl.  Keep the legacy ioctl above for older
 * images, then use the kernel's generic-netlink interface directly rather
 * than depending on libnl or an optional `iw` binary. */
static size_t nl_align(size_t length)
{
    return (length + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1);
}

static int nl_put(unsigned char *buffer, size_t capacity, size_t *used,
                  uint16_t type, const void *value, size_t value_length)
{
    struct nlattr *attribute;
    size_t length = NLA_HDRLEN + value_length;
    size_t aligned = nl_align(length);

    if (!buffer || !used || *used > capacity || aligned > capacity - *used ||
        length > UINT16_MAX)
        return -1;
    attribute = (struct nlattr *)(buffer + *used);
    attribute->nla_type = type;
    attribute->nla_len = (uint16_t)length;
    if (value_length)
        memcpy((unsigned char *)attribute + NLA_HDRLEN, value, value_length);
    if (aligned > length)
        memset((unsigned char *)attribute + length, 0, aligned - length);
    *used += aligned;
    return 0;
}

static int nl_put_u32(unsigned char *buffer, size_t capacity, size_t *used,
                      uint16_t type, uint32_t value)
{
    return nl_put(buffer, capacity, used, type, &value, sizeof(value));
}

static const struct nlattr *nl_find(const void *payload, size_t length,
                                    uint16_t wanted)
{
    const unsigned char *cursor = payload;

    while (length >= NLA_HDRLEN) {
        const struct nlattr *attribute = (const struct nlattr *)cursor;
        size_t aligned;
        if (attribute->nla_len < NLA_HDRLEN)
            return NULL;
        aligned = nl_align(attribute->nla_len);
        if (aligned > length)
            return NULL;
        if ((attribute->nla_type & NLA_TYPE_MASK) == wanted)
            return attribute;
        cursor += aligned;
        length -= aligned;
    }
    return NULL;
}

static int nl_wait_ack(int fd, unsigned char *buffer, size_t capacity,
                       int timeout_ms)
{
    struct pollfd descriptor = { fd, POLLIN, 0 };
    long long deadline = monotonic_ms() + timeout_ms;

    for (;;) {
        ssize_t received;
        struct nlmsghdr *header;
        int remaining;
        int wait_ms = (int)(deadline - monotonic_ms());
        if (wait_ms <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (poll(&descriptor, 1, wait_ms) <= 0) {
            errno = errno == EINTR ? EINTR : ETIMEDOUT;
            if (errno == EINTR)
                continue;
            return -1;
        }
        received = recv(fd, buffer, capacity, 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0)
            return -1;
        remaining = (int)received;
        for (header = (struct nlmsghdr *)buffer;
             NLMSG_OK(header, remaining);
             header = NLMSG_NEXT(header, remaining)) {
            if (header->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *error = (struct nlmsgerr *)NLMSG_DATA(header);
                if (header->nlmsg_len < NLMSG_LENGTH(sizeof(*error))) {
                    errno = EPROTO;
                    return -1;
                }
                if (!error->error)
                    return 0;
                errno = -error->error;
                return -1;
            }
        }
    }
}

static int nl80211_family_id(int fd, unsigned char *buffer, size_t capacity)
{
    struct nlmsghdr *header = (struct nlmsghdr *)buffer;
    struct genlmsghdr *generic;
    struct sockaddr_nl address;
    size_t used = NLMSG_LENGTH(GENL_HDRLEN);
    uint32_t sequence = 1;
    int remaining;
    ssize_t received;
    struct pollfd descriptor = { fd, POLLIN, 0 };

    memset(buffer, 0, capacity);
    header->nlmsg_len = (uint32_t)used;
    header->nlmsg_type = GENL_ID_CTRL;
    header->nlmsg_flags = NLM_F_REQUEST;
    header->nlmsg_seq = sequence;
    generic = (struct genlmsghdr *)NLMSG_DATA(header);
    generic->cmd = CTRL_CMD_GETFAMILY;
    generic->version = 1;
    if (nl_put(buffer, capacity, &used, CTRL_ATTR_FAMILY_NAME,
               "nl80211", sizeof("nl80211")) < 0)
        return -1;
    header->nlmsg_len = (uint32_t)used;
    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    if (sendto(fd, buffer, used, 0, (struct sockaddr *)&address,
               sizeof(address)) < 0)
        return -1;
    if (poll(&descriptor, 1, NL80211_SCAN_TIMEOUT_MS) <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    received = recv(fd, buffer, capacity, 0);
    if (received < 0)
        return -1;
    remaining = (int)received;
    for (header = (struct nlmsghdr *)buffer;
         NLMSG_OK(header, remaining);
         header = NLMSG_NEXT(header, remaining)) {
        const struct nlattr *attribute;
        size_t payload_length;
        if (header->nlmsg_type == NLMSG_ERROR) {
            struct nlmsgerr *error = (struct nlmsgerr *)NLMSG_DATA(header);
            errno = error->error ? -error->error : EPROTO;
            return -1;
        }
        if (header->nlmsg_seq != sequence ||
            header->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN))
            continue;
        generic = (struct genlmsghdr *)NLMSG_DATA(header);
        payload_length = header->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
        attribute = nl_find((unsigned char *)generic + GENL_HDRLEN,
                            payload_length, CTRL_ATTR_FAMILY_ID);
        if (attribute && attribute->nla_len >= NLA_HDRLEN + sizeof(uint16_t)) {
            uint16_t family;
            memcpy(&family, (unsigned char *)attribute + NLA_HDRLEN,
                   sizeof(family));
            return (int)family;
        }
    }
    errno = EPROTO;
    return -1;
}

static int nl80211_send_request(int fd, int family, uint8_t command,
                                uint16_t flags, unsigned char *buffer,
                                size_t capacity, size_t used)
{
    struct nlmsghdr *header = (struct nlmsghdr *)buffer;
    struct genlmsghdr *generic = (struct genlmsghdr *)NLMSG_DATA(header);
    struct sockaddr_nl address;

    header->nlmsg_len = (uint32_t)used;
    header->nlmsg_type = (uint16_t)family;
    header->nlmsg_flags = flags;
    header->nlmsg_seq = 2;
    generic->cmd = command;
    generic->version = 1;
    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    if (sendto(fd, buffer, used, 0, (struct sockaddr *)&address,
               sizeof(address)) < 0)
        return -1;
    if (flags & NLM_F_ACK)
        return nl_wait_ack(fd, buffer, capacity, NL80211_SCAN_TIMEOUT_MS);
    return 0;
}

static int nl80211_trigger_scan(int fd, int family, unsigned int ifindex,
                                unsigned char *buffer, size_t capacity)
{
    size_t used = NLMSG_LENGTH(GENL_HDRLEN);
    size_t nested_start;
    struct nlattr *nested;
    int result;

    memset(buffer, 0, capacity);
    if (nl_put_u32(buffer, capacity, &used, NL80211_ATTR_IFINDEX,
                   ifindex) < 0)
        return -1;
    nested_start = used;
    nested = (struct nlattr *)(buffer + used);
    nested->nla_type = NL80211_ATTR_SCAN_SSIDS | NLA_F_NESTED;
    nested->nla_len = NLA_HDRLEN;
    used += NLA_ALIGN(NLA_HDRLEN);
    if (nl_put(buffer, capacity, &used, 1, NULL, 0) < 0)
        return -1;
    nested->nla_len = (uint16_t)(used - nested_start);
    result = nl80211_send_request(fd, family, NL80211_CMD_TRIGGER_SCAN,
                                  NLM_F_REQUEST | NLM_F_ACK, buffer,
                                  capacity, used);
    if (result < 0 && errno == EBUSY)
        return 0;
    return result;
}

static void nl80211_parse_ies(const unsigned char *ies, size_t length,
                              char *ssid, size_t ssid_size, int *encrypted)
{
    size_t offset = 0;
    int has_rsn = 0, has_wpa = 0;

    if (ssid_size)
        ssid[0] = '\0';
    if (encrypted)
        *encrypted = 0;
    while (ies && offset + 2 <= length) {
        unsigned int id = ies[offset];
        size_t item_length = ies[offset + 1];
        if (offset + 2 + item_length > length)
            break;
        if (id == 0 && ssid_size) {
            size_t copy = item_length < ssid_size - 1 ? item_length : ssid_size - 1;
            memcpy(ssid, ies + offset + 2, copy);
            ssid[copy] = '\0';
        } else if (id == 48) {
            has_rsn = 1;
        } else if (id == 221 && item_length >= 4 &&
                   !memcmp(ies + offset + 2, "\x00\x50\xf2\x01", 4)) {
            has_wpa = 1;
        }
        offset += 2 + item_length;
    }
    if (encrypted)
        *encrypted = has_rsn || has_wpa;
}

static int nl80211_append_bss(const struct nlattr *bss,
                              struct scan_result *result)
{
    const struct nlattr *bssid, *signal, *signal_unspec, *frequency, *ies, *beacon;
    char ssid[IW_ESSID_MAX_SIZE + 1];
    int encrypted = 0;
    int signal_dbm = -100;
    int signal_percent = -1;
    int frequency_mhz = 0;
    const void *payload;
    size_t payload_length;

    if (!bss || bss->nla_len < NLA_HDRLEN)
        return -1;
    payload = (unsigned char *)bss + NLA_HDRLEN;
    payload_length = bss->nla_len - NLA_HDRLEN;
    bssid = nl_find(payload, payload_length, NL80211_BSS_BSSID);
    ies = nl_find(payload, payload_length, NL80211_BSS_INFORMATION_ELEMENTS);
    beacon = nl_find(payload, payload_length, NL80211_BSS_BEACON_IES);
    if (!ies)
        ies = beacon;
    if (!bssid || !ies || bssid->nla_len < NLA_HDRLEN + 6)
        return 0;
    payload = (unsigned char *)ies + NLA_HDRLEN;
    payload_length = ies->nla_len - NLA_HDRLEN;
    nl80211_parse_ies(payload, payload_length, ssid, sizeof(ssid), &encrypted);
    if (!ssid[0])
        return 0;
    signal = nl_find((unsigned char *)bss + NLA_HDRLEN,
                     bss->nla_len - NLA_HDRLEN, NL80211_BSS_SIGNAL_MBM);
    signal_unspec = nl_find((unsigned char *)bss + NLA_HDRLEN,
                            bss->nla_len - NLA_HDRLEN,
                            NL80211_BSS_SIGNAL_UNSPEC);
    frequency = nl_find((unsigned char *)bss + NLA_HDRLEN,
                        bss->nla_len - NLA_HDRLEN, NL80211_BSS_FREQUENCY);
    if (frequency && frequency->nla_len >= NLA_HDRLEN + sizeof(uint32_t))
        memcpy(&frequency_mhz, (unsigned char *)frequency + NLA_HDRLEN,
               sizeof(frequency_mhz));
    if (signal && signal->nla_len >= NLA_HDRLEN + sizeof(int32_t)) {
        memcpy(&signal_dbm, (unsigned char *)signal + NLA_HDRLEN,
               sizeof(signal_dbm));
        signal_dbm /= 100;
    } else {
        signal = NULL;
    }
    if (!signal && signal_unspec &&
        signal_unspec->nla_len >= NLA_HDRLEN + sizeof(uint8_t)) {
        uint8_t value;
        memcpy(&value, (unsigned char *)signal_unspec + NLA_HDRLEN,
               sizeof(value));
        signal_percent = value > 100 ? 100 : value;
    }
    if (!result)
        return -1;
    if (!signal && signal_percent < 0)
        return 0;
    memset(result, 0, sizeof(*result));
    copy_string(result->ssid, sizeof(result->ssid), ssid);
    result->signal = signal_dbm;
    result->signal_percent = signal_percent;
    result->frequency = frequency_mhz;
    result->five_ghz = frequency_mhz >= 5000 && frequency_mhz < 6000;
    if (encrypted)
        copy_string(result->flags, sizeof(result->flags), "WPA2");
    return 1;
}

static int nl80211_wait_for_scan_event(int fd, unsigned char *buffer,
                                       size_t capacity, long long deadline)
{
    struct pollfd descriptor = { fd, POLLIN, 0 };

    for (;;) {
        struct nlmsghdr *header;
        int remaining;
        ssize_t received;
        int wait_ms = (int)(deadline - monotonic_ms());

        if (wait_ms <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (poll(&descriptor, 1, wait_ms) <= 0) {
            if (errno == EINTR)
                continue;
            errno = ETIMEDOUT;
            return -1;
        }
        received = recv(fd, buffer, capacity, 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0)
            return -1;
        remaining = (int)received;
        for (header = (struct nlmsghdr *)buffer;
             NLMSG_OK(header, remaining);
             header = NLMSG_NEXT(header, remaining)) {
            struct genlmsghdr *generic;
            const struct nlattr *attrs;
            size_t payload_length;

            if (header->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *error = (struct nlmsgerr *)NLMSG_DATA(header);
                errno = error->error ? -error->error : EPROTO;
                return -1;
            }
            if (header->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN))
                continue;
            generic = (struct genlmsghdr *)NLMSG_DATA(header);
            payload_length = header->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
            attrs = (const struct nlattr *)((unsigned char *)generic + GENL_HDRLEN);
            if (generic->cmd == NL80211_CMD_NEW_SCAN_RESULTS)
                return 0;
            if (generic->cmd == NL80211_CMD_SCAN_ABORTED) {
                errno = ECANCELED;
                return -1;
            }
            (void)attrs;
            (void)payload_length;
        }
    }
}

static int nl80211_dump_scan(int fd, int family, unsigned int ifindex,
                             unsigned char *buffer, size_t capacity,
                             char *data, size_t data_size)
{
    struct nlmsghdr *header;
    struct genlmsghdr *generic;
    size_t used = NLMSG_LENGTH(GENL_HDRLEN);
    long long deadline = monotonic_ms() + NL80211_SCAN_RETRY_MS;
    int remaining, result_count = 0;
    struct scan_result results[SCAN_MAX];
    ssize_t received;
    struct pollfd descriptor = { fd, POLLIN, 0 };

    memset(buffer, 0, capacity);
    if (nl_put_u32(buffer, capacity, &used, NL80211_ATTR_IFINDEX,
                   ifindex) < 0)
        return -1;
    header = (struct nlmsghdr *)buffer;
    header->nlmsg_len = (uint32_t)used;
    header->nlmsg_type = (uint16_t)family;
    header->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    header->nlmsg_seq = 3;
    generic = (struct genlmsghdr *)NLMSG_DATA(header);
    generic->cmd = NL80211_CMD_GET_SCAN;
    generic->version = 1;
    if (send(fd, buffer, used, 0) < 0)
        return -1;
    for (;;) {
        int wait_ms = (int)(deadline - monotonic_ms());
        if (wait_ms <= 0) {
            errno = EAGAIN;
            return -1;
        }
        if (poll(&descriptor, 1, wait_ms) <= 0) {
            errno = errno == EINTR ? EINTR : EAGAIN;
            if (errno == EINTR)
                continue;
            return -1;
        }
        received = recv(fd, buffer, capacity, 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            errno = EAGAIN;
            continue;
        }
        if (received < 0)
            return -1;
        remaining = (int)received;
        for (header = (struct nlmsghdr *)buffer;
             NLMSG_OK(header, remaining);
             header = NLMSG_NEXT(header, remaining)) {
            size_t payload_length;
            const struct nlattr *bss;
            if (header->nlmsg_type == NLMSG_DONE) {
                int i, j;
                for (i = 0; i < result_count; ++i)
                    for (j = i + 1; j < result_count; ++j)
                        if (scan_result_better(&results[j], &results[i])) {
                            struct scan_result swap = results[i];
                            results[i] = results[j];
                            results[j] = swap;
                        }
                return serialize_scan_results(results, result_count,
                                               data, data_size);
            }
            if (header->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *error = (struct nlmsgerr *)NLMSG_DATA(header);
                errno = error->error ? -error->error : EPROTO;
                return -1;
            }
            if (header->nlmsg_len < NLMSG_LENGTH(GENL_HDRLEN))
                continue;
            generic = (struct genlmsghdr *)NLMSG_DATA(header);
            payload_length = header->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
            bss = nl_find((unsigned char *)generic + GENL_HDRLEN,
                          payload_length, NL80211_ATTR_BSS);
            if (bss && result_count < SCAN_MAX) {
                int parsed = nl80211_append_bss(bss, &results[result_count]);
                if (parsed < 0)
                    return -1;
                if (parsed > 0)
                    ++result_count;
            }
        }
    }
}

static int nl80211_scan(const char *iface, char *data, size_t data_size)
{
    unsigned char *buffer;
    struct sockaddr_nl address;
    unsigned int ifindex;
    long long deadline;
    int fd, family, result;

    if (!iface || !data || data_size < 16)
        return -EINVAL;
    ifindex = if_nametoindex(iface);
    if (!ifindex)
        return -errno;
    fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (fd < 0)
        return -errno;
    memset(&address, 0, sizeof(address));
    address.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        result = -errno;
        close(fd);
        return result;
    }
    buffer = malloc(NL80211_BUFFER_SIZE);
    if (!buffer) {
        close(fd);
        return -ENOMEM;
    }
    family = nl80211_family_id(fd, buffer, NL80211_BUFFER_SIZE);
    if (family < 0 || nl80211_trigger_scan(fd, family, ifindex, buffer,
                                           NL80211_BUFFER_SIZE) < 0) {
        result = -errno;
        goto done;
    }
    deadline = monotonic_ms() + NL80211_SCAN_TIMEOUT_MS;
    if (nl80211_wait_for_scan_event(fd, buffer, NL80211_BUFFER_SIZE,
                                    deadline) < 0) {
        result = -errno;
        goto done;
    }
    for (;;) {
        result = nl80211_dump_scan(fd, family, ifindex, buffer,
                                   NL80211_BUFFER_SIZE, data, data_size);
        if (result >= 0)
            break;
        if (errno != EAGAIN && errno != EBUSY)
            break;
        if (monotonic_ms() >= deadline) {
            result = -ETIMEDOUT;
            break;
        }
        (void)poll(NULL, 0, NL80211_SCAN_RETRY_MS);
    }
done:
    free(buffer);
    close(fd);
    return result;
}

static void finish_scan(struct daemon_ctx *ctx, int failed, const char *error)
{
    int fd = ctx->scan.client_fd;
    unsigned long id = ctx->scan.id;
    char reply[WPA_REPLY_MAX], data[LE_ADAPTER_MSG_MAX];
    int ci, n;

    ctx->scan.active = 0;
    ctx->scan.client_fd = -1;
    if (fd < 0)
        return;
    ci = client_index(ctx, fd);
    if (ci < 0)
        return;
    if (failed)
        (void)send_err_fd(fd, id, error ? error : "scan failed");
    else {
        n = wpa_call(ctx, "SCAN_RESULTS\n", reply, sizeof(reply));
        if (n < 0) {
            le_log_error("networkd: scan results request failed");
            (void)send_err_fd(fd, id, "unable to read scan results");
        } else if (n >= 15 && !strncmp(reply, "UNKNOWN COMMAND", 15)) {
            le_log_error("networkd: scan results command unavailable");
            (void)send_err_fd(fd, id, "scan results unavailable");
        } else if (parse_scan_results(reply, data, sizeof(data)) < 0) {
            le_log_error("networkd: scan results parse failed (%d bytes)", n);
            (void)send_err_fd(fd, id, "unable to read scan results");
        } else {
            le_log_info("networkd: scan results reply received (%d bytes)", n);
            (void)send_ok_fd(fd, id, data);
        }
    }
    ctx->clients[ci].busy = 0;
}

static void check_scan_timeout(struct daemon_ctx *ctx)
{
    long long now = monotonic_ms();
    if (!ctx->scan.active)
        return;
    if (ctx->wpa.monitor.fd < 0 && now >= ctx->scan.poll_at) {
        finish_scan(ctx, 0, NULL);
        return;
    }
    if (now >= ctx->scan.deadline)
        finish_scan(ctx, 1, "scan timed out");
}

static void handle_wpa_event(struct daemon_ctx *ctx, const char *event)
{
    struct network_state before;
    le_log_debug("networkd: wpa event: %.80s", event);
    if (strstr(event, "CTRL-EVENT-SCAN-RESULTS")) {
        if (ctx->scan.active)
            finish_scan(ctx, 0, NULL);
        return;
    }
    if (strstr(event, "CTRL-EVENT-CONNECTED") ||
        strstr(event, "CTRL-EVENT-DISCONNECTED") ||
        strstr(event, "CTRL-EVENT-SSID-TEMP-DISABLED") ||
        strstr(event, "CTRL-EVENT-SSID-REENABLED")) {
        before = ctx->state;
        refresh_state(ctx);
        if (!state_equal(&before, &ctx->state))
            broadcast_state(ctx, "network_changed");
    }
}

static void drain_wpa_events(struct daemon_ctx *ctx)
{
    char event[WPA_REPLY_MAX];
    int n;
    while (ctx->wpa.monitor.fd >= 0) {
        n = wpa_ctrl_recv(&ctx->wpa.monitor, event, sizeof(event));
        if (n >= 0) {
            handle_wpa_event(ctx, event);
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        wpa_close(ctx);
        copy_string(ctx->state.state, sizeof(ctx->state.state), "unavailable");
        break;
    }
}

/* ----- Netlink ---------------------------------------------------------- */

static int open_netlink(void)
{
    struct sockaddr_nl addr;
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        return -1;
    set_cloexec(fd);
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        set_nonblock(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void handle_netlink(struct daemon_ctx *ctx)
{
    char buffer[8192];
    ssize_t n;
    unsigned int wanted = if_nametoindex(ctx->interface);
    n = recv(ctx->netlink_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    while (n > 0) {
        struct nlmsghdr *nlh;
        unsigned int length = (unsigned int)n;
        int relevant = 0;
        for (nlh = (struct nlmsghdr *)buffer; NLMSG_OK(nlh, length);
             nlh = NLMSG_NEXT(nlh, length)) {
            if (nlh->nlmsg_type == RTM_NEWLINK || nlh->nlmsg_type == RTM_DELLINK) {
                struct ifinfomsg *ifi = (struct ifinfomsg *)NLMSG_DATA(nlh);
                if ((unsigned int)ifi->ifi_index == wanted) {
                    int up = nlh->nlmsg_type == RTM_NEWLINK &&
                             (ifi->ifi_flags & IFF_UP) &&
                             (ifi->ifi_flags & IFF_RUNNING);
                    if (ctx->state.link_up != up)
                        relevant = 1;
                }
            } else if (nlh->nlmsg_type == RTM_NEWADDR ||
                       nlh->nlmsg_type == RTM_DELADDR) {
                struct ifaddrmsg *ifa = (struct ifaddrmsg *)NLMSG_DATA(nlh);
                if (ifa->ifa_index == wanted)
                    relevant = 1;
            }
        }
        if (relevant)
            refresh_and_broadcast(ctx, "network_changed");
        n = recv(ctx->netlink_fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    }
}

/* ----- Request dispatch ------------------------------------------------- */

static int json_string_arg(const char *args, const char *key, char *out,
                           size_t size)
{
    const char *p, *end;
    size_t n = 0;
    char c;
    char needle[64];
    if (!args || !out || !size)
        return -1;
    if (snprintf(needle, sizeof(needle), "\"%s\"", key) >= (int)sizeof(needle))
        return -1;
    p = strstr(args, needle);
    if (!p)
        return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (*p++ != ':')
        return -1;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (*p++ != '"')
        return -1;
    end = p;
    while (*end && *end != '"') {
        if (*end == '\\' && end[1])
            ++end;
        ++end;
    }
    if (*end != '"')
        return -1;
    while (p < end) {
        c = *p++;
        if (c == '\\') {
            c = *p++;
            if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c == 't') c = '\t';
            else if (c != '"' && c != '\\' && c != '/') return -1;
        }
        if ((unsigned char)c < 32 || n + 1 >= size)
            return -1;
        out[n++] = c;
    }
    out[n] = '\0';
    return 1;
}

static int wpa_quote(char *out, size_t size, const char *value)
{
    size_t n = 0;
    const unsigned char *p = (const unsigned char *)value;
    if (n + 1 >= size) return -1;
    out[n++] = '"';
    while (*p) {
        if (*p == '"' || *p == '\\') {
            if (n + 2 >= size) return -1;
            out[n++] = '\\';
        }
        if (*p < 32 || n + 1 >= size) return -1;
        out[n++] = *p++;
    }
    out[n++] = '"';
    out[n] = '\0';
    return 0;
}

static int existing_network_id(struct daemon_ctx *ctx)
{
    char reply[WPA_REPLY_MAX], *line, *next, *field, *fields[4];
    int id, field_count;
    if (wpa_call(ctx, "LIST_NETWORKS\n", reply, sizeof(reply)) < 0)
        return -1;
    line = reply;
    while (line && *line) {
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        field_count = 0;
        field = line;
        while (field_count < 4) {
            fields[field_count++] = field;
            field = strchr(field, '\t');
            if (!field)
                break;
            *field++ = '\0';
        }
        if (field_count == 4 && sscanf(fields[0], "%d", &id) == 1 &&
            strstr(fields[3], "[CURRENT]"))
            return id;
        line = next;
    }
    return -1;
}

static void remove_network_profile(struct daemon_ctx *ctx, int id)
{
    char command[64], reply[WPA_REPLY_MAX];
    if (id < 0 || snprintf(command, sizeof(command), "REMOVE_NETWORK %d\n", id) >=
                      (int)sizeof(command))
        return;
    (void)wpa_ok(ctx, command, reply, sizeof(reply));
}

static void restore_network_profile(struct daemon_ctx *ctx, int previous,
                                    int candidate)
{
    char command[64], reply[WPA_REPLY_MAX];
    if (candidate >= 0)
        remove_network_profile(ctx, candidate);
    if (previous >= 0 &&
        snprintf(command, sizeof(command), "SELECT_NETWORK %d\n", previous) <
            (int)sizeof(command)) {
        (void)wpa_ok(ctx, command, reply, sizeof(reply));
        (void)wpa_ok(ctx, "SAVE_CONFIG\n", reply, sizeof(reply));
        ctx->network_id = previous;
    } else {
        (void)wpa_ok(ctx, "DISCONNECT\n", reply, sizeof(reply));
        ctx->network_id = -1;
    }
}

static void restore_previous_network(struct daemon_ctx *ctx)
{
    restore_network_profile(ctx, ctx->association.previous_network_id,
                            ctx->association.candidate_network_id);
}

static int connect_network(struct daemon_ctx *ctx, const char *ssid,
                           const char *psk, const char *security,
                           int *previous_id)
{
    char reply[WPA_REPLY_MAX], quoted[512], command[768], value[32];
    int id;
    const char *key_mgmt;

    *previous_id = -1;
    if (wpa_call(ctx, "STATUS\n", reply, sizeof(reply)) >= 0 &&
        wpa_value(reply, "id", value, sizeof(value)))
        *previous_id = (int)strtol(value, NULL, 10);
    if (*previous_id < 0)
        *previous_id = existing_network_id(ctx);
    if (wpa_ok(ctx, "DISABLE_NETWORK all\n", reply, sizeof(reply)) < 0) {
        restore_network_profile(ctx, *previous_id, -1);
        return -1;
    }
    if (wpa_call(ctx, "ADD_NETWORK", reply, sizeof(reply)) < 0) {
        restore_network_profile(ctx, *previous_id, -1);
        return -2;
    }
    le_log_info("networkd: ADD_NETWORK reply first=%02x len=%zu", (unsigned char)reply[0], strlen(reply));
    id = (int)strtol(reply, NULL, 10);
    if (id < 0 || (reply[0] < '0' || reply[0] > '9')) {
        restore_network_profile(ctx, *previous_id, -1);
        return -3;
    }
    if (wpa_quote(quoted, sizeof(quoted), ssid) < 0 ||
        snprintf(command, sizeof(command), "SET_NETWORK %d ssid %s\n", id,
                 quoted) >= (int)sizeof(command) ||
        wpa_ok(ctx, command, reply, sizeof(reply)) < 0) {
        restore_network_profile(ctx, *previous_id, id);
        return -4;
    }

    if (!security || !strcmp(security, "open") || !strcmp(security, "none"))
        key_mgmt = "NONE";
    else
        key_mgmt = "WPA-PSK";
    if (snprintf(command, sizeof(command), "SET_NETWORK %d key_mgmt %s\n", id,
                 key_mgmt) >= (int)sizeof(command) ||
        wpa_ok(ctx, command, reply, sizeof(reply)) < 0) {
        restore_network_profile(ctx, *previous_id, id);
        return -5;
    }
    if (strcmp(key_mgmt, "NONE")) {
        if (!psk || !*psk || wpa_quote(quoted, sizeof(quoted), psk) < 0 ||
            snprintf(command, sizeof(command), "SET_NETWORK %d psk %s\n", id,
                     quoted) >= (int)sizeof(command) ||
            wpa_ok(ctx, command, reply, sizeof(reply)) < 0) {
            restore_network_profile(ctx, *previous_id, id);
            return -6;
        }
    }
    if (snprintf(command, sizeof(command), "ENABLE_NETWORK %d\n", id) >=
            (int)sizeof(command) || wpa_ok(ctx, command, reply, sizeof(reply)) < 0 ||
        snprintf(command, sizeof(command), "SELECT_NETWORK %d\n", id) >=
            (int)sizeof(command) || wpa_ok(ctx, command, reply, sizeof(reply)) < 0) {
        restore_network_profile(ctx, *previous_id, id);
        return -7;
    }
    return id;
}

static void finish_association(struct daemon_ctx *ctx, int success)
{
    char reply[WPA_REPLY_MAX];
    int fd = ctx->association.client_fd;
    int ci = client_index(ctx, fd);
    int candidate = ctx->association.candidate_network_id;
    unsigned long id = ctx->association.id;

    if (!success) {
        le_log_error("networkd: Wi-Fi association did not complete for ssid=\"%s\"",
                     ctx->association.ssid);
        restore_previous_network(ctx);
        if (ci >= 0) {
            (void)send_err_fd(fd, id, "Wi-Fi association did not complete");
            ctx->clients[ci].busy = 0;
        }
        memset(&ctx->association, 0, sizeof(ctx->association));
        ctx->association.client_fd = -1;
        return;
    }

    if (wpa_ok(ctx, "SAVE_CONFIG\n", reply, sizeof(reply)) < 0) {
        restore_previous_network(ctx);
        if (ci >= 0) {
            (void)send_err_fd(fd, id, "Wi-Fi profile could not be saved");
            ctx->clients[ci].busy = 0;
        }
        memset(&ctx->association, 0, sizeof(ctx->association));
        ctx->association.client_fd = -1;
        return;
    }
    ctx->network_id = candidate;
    reset_network_health(ctx, monotonic_ms());
    copy_string(ctx->state.ssid, sizeof(ctx->state.ssid), ctx->association.ssid);
    copy_string(ctx->state.state, sizeof(ctx->state.state), "connecting");
    ctx->association.active = 0;
    if (start_dhcp(ctx, 0, fd, id) < 0) {
        restore_previous_network(ctx);
        if (ci >= 0) {
            ctx->clients[ci].busy = 0;
            (void)send_err_fd(fd, id, "unable to start DHCP");
        }
        memset(&ctx->association, 0, sizeof(ctx->association));
        ctx->association.client_fd = -1;
    }
}

static void check_association(struct daemon_ctx *ctx, long long now)
{
    char reply[WPA_REPLY_MAX], state[32], value[32];
    int completed = 0;

    if (!ctx->association.active || now < ctx->association.poll_at)
        return;
    if (wpa_call(ctx, "STATUS\n", reply, sizeof(reply)) >= 0 &&
        wpa_value(reply, "wpa_state", state, sizeof(state)) &&
        !strcmp(state, "COMPLETED") &&
        wpa_value(reply, "id", value, sizeof(value)) &&
        (int)strtol(value, NULL, 10) == ctx->association.candidate_network_id)
        completed = 1;
    if (completed)
        finish_association(ctx, 1);
    else if (now >= ctx->association.deadline)
        finish_association(ctx, 0);
    else
        ctx->association.poll_at = now + ASSOCIATION_POLL_MS;
}

static void cancel_association(struct daemon_ctx *ctx)
{
    if (!ctx->association.active)
        return;
    restore_previous_network(ctx);
    if (ctx->association.client_fd >= 0) {
        int ci = client_index(ctx, ctx->association.client_fd);
        if (ci >= 0) {
            (void)send_err_fd(ctx->association.client_fd, ctx->association.id,
                              "Wi-Fi association was cancelled");
            ctx->clients[ci].busy = 0;
        }
    }
    memset(&ctx->association, 0, sizeof(ctx->association));
    ctx->association.client_fd = -1;
}

static void dispatch_request(struct daemon_ctx *ctx, int ci, char *message)
{
    char cmd[64], *args = NULL;
    unsigned long id = 0;
    char data[LE_ADAPTER_MSG_MAX];
    int result;

    result = le_adapter_parse_request(message, cmd, sizeof(cmd), &args, &id);
    if (result < 0) {
        le_log_warn("networkd: malformed request from client %d", ci);
        (void)send_err_fd(ctx->clients[ci].fd, 0, "malformed request");
        return;
    }
    le_log_debug("networkd: cmd=\"%s\" id=%lu client=%d", cmd, id, ci);
    if (!strcmp(cmd, "status")) {
        (void)ensure_wpa(ctx);
        refresh_state(ctx);
        if (state_json(&ctx->state, data, sizeof(data)) < 0 ||
            send_ok_fd(ctx->clients[ci].fd, id, data) < 0)
            remove_client(ctx, ci);
    } else if (!strcmp(cmd, "scan")) {
        char reply[WPA_REPLY_MAX];
        int scan_state;
        if (ctx->scan.active) {
            (void)send_err_fd(ctx->clients[ci].fd, id, "scan already in progress");
            return;
        }
        le_log_info("networkd: scan requested");
        scan_state = wpa_scan_request(ctx, reply, sizeof(reply));
        if (scan_state < 0) {
            le_log_error("networkd: wpa_supplicant scan unavailable");
            (void)send_err_fd(ctx->clients[ci].fd, id, "wpa_supplicant scan unavailable");
            return;
        }
        if (scan_state == 1)
            le_log_warn("networkd: scan already active; waiting for results");
        else if (scan_state == 2) {
            int wext_result;
            le_log_warn("networkd: wpa scan unsupported; trying WEXT driver results");
            wext_result = wext_scan(ctx->interface, data, sizeof(data));
            if (wext_result >= 0) {
                le_log_info("networkd: WEXT scan results ready");
                (void)send_ok_fd(ctx->clients[ci].fd, id, data);
            } else if (wext_result == -EOPNOTSUPP ||
                       wext_result == -ENOTSUP) {
                le_log_warn("networkd: WEXT scan returned EOPNOTSUPP; using nl80211");
                if (nl80211_scan(ctx->interface, data, sizeof(data)) < 0) {
                    le_log_error("networkd: nl80211 scan unavailable: %s",
                                 strerror(errno));
                    (void)send_err_fd(ctx->clients[ci].fd, id,
                                      "Wi-Fi scan is unavailable");
                } else {
                    le_log_info("networkd: nl80211 scan results ready");
                    (void)send_ok_fd(ctx->clients[ci].fd, id, data);
                }
            } else {
                le_log_error("networkd: WEXT scan unavailable: %s",
                             strerror(-wext_result));
                (void)send_err_fd(ctx->clients[ci].fd, id,
                                  "Wi-Fi scan is unavailable");
            }
            return;
        }
        ctx->scan.active = 1;
        ctx->scan.client_fd = ctx->clients[ci].fd;
        ctx->scan.id = id;
        ctx->scan.deadline = monotonic_ms() + SCAN_TIMEOUT_MS;
        ctx->scan.poll_at = monotonic_ms() + SCAN_POLL_MS;
        ctx->clients[ci].busy = 1;
    } else if (!strcmp(cmd, "connect")) {
        char ssid[256], psk[256], security[64];
        int have_ssid = json_string_arg(args, "ssid", ssid, sizeof(ssid));
        int have_psk = json_string_arg(args, "psk", psk, sizeof(psk));
        int have_security = json_string_arg(args, "security", security, sizeof(security));
        int connect_result;
        if (have_ssid != 1 || have_psk < 0 || have_security < 0) {
            (void)send_err_fd(ctx->clients[ci].fd, id, "connect requires ssid, psk, security");
            return;
        }
        if (ctx->dhcp.active || ctx->association.active) {
            (void)send_err_fd(ctx->clients[ci].fd, id,
                              ctx->dhcp.active ? "DHCP already in progress" :
                              "Wi-Fi association already in progress");
            return;
        }
        le_log_info("networkd: connect to ssid=\"%s\" security=%s", ssid, have_security == 1 ? security : "wpa2");
        {
            int previous_id = -1;
            connect_result = connect_network(ctx, ssid, have_psk == 1 ? psk : "",
                                             have_security == 1 ? security : "wpa2",
                                             &previous_id);
            if (connect_result >= 0) {
                long long now = monotonic_ms();
                ctx->association.active = 1;
                ctx->association.client_fd = ctx->clients[ci].fd;
                ctx->association.id = id;
                ctx->association.previous_network_id = previous_id;
                ctx->association.candidate_network_id = connect_result;
                ctx->association.deadline = now + ASSOCIATION_TIMEOUT_MS;
                ctx->association.poll_at = now;
                copy_string(ctx->association.ssid,
                            sizeof(ctx->association.ssid), ssid);
                ctx->clients[ci].busy = 1;
                return;
            }
        }
        if (connect_result < 0) {
            le_log_error("networkd: wpa_supplicant rejected network \\\"%s\\\" at stage %d", ssid, connect_result);
            (void)send_err_fd(ctx->clients[ci].fd, id,
                              connect_result == -2 ? "wpa_supplicant unavailable" :
                              connect_result == -4 ? "SSID rejected by wpa_supplicant" :
                              connect_result == -5 ? "WPA2 security rejected by wpa_supplicant" :
                              connect_result == -6 ? "Wi-Fi password rejected by wpa_supplicant" :
                              connect_result == -7 ? "Wi-Fi network could not be enabled or selected" :
                              "wpa_supplicant rejected the network");
            return;
        }
    } else if (!strcmp(cmd, "disconnect")) {
        char reply[WPA_REPLY_MAX];
        le_log_info("networkd: disconnect requested");
        if (ctx->association.active)
            cancel_association(ctx);
        if (ctx->dhcp.active)
            cancel_dhcp(ctx);
        if (ctx->wpa.command.fd >= 0)
            (void)wpa_ok(ctx, "DISABLE_NETWORK all\n", reply, sizeof(reply));
        reset_network_health(ctx, monotonic_ms());
        ctx->state.ssid[0] = '\0';
        ctx->state.ip[0] = '\0';
        ctx->state.gateway[0] = '\0';
        ctx->state.dns[0] = '\0';
        copy_string(ctx->state.state, sizeof(ctx->state.state), "disconnected");
        ctx->clients[ci].busy = 1;
        if (start_dhcp(ctx, 1, ctx->clients[ci].fd, id) < 0) {
            ctx->clients[ci].busy = 0;
            (void)send_ok_fd(ctx->clients[ci].fd, id, "{}");
        }
    } else if (!strcmp(cmd, "set_hostname")) {
        char hostname[HOST_NAME_MAX + 1];
        int have = json_string_arg(args, "hostname", hostname, sizeof(hostname));
        int fd;
        ssize_t n;
        if (have != 1 || !hostname[0]) {
            (void)send_err_fd(ctx->clients[ci].fd, id, "hostname is required");
            return;
        }
        fd = open("/proc/sys/kernel/hostname", O_WRONLY | O_CLOEXEC);
        n = fd >= 0 ? write(fd, hostname, strlen(hostname)) : -1;
        if (fd >= 0) close(fd);
        if (n < 0 || (size_t)n != strlen(hostname))
            (void)send_err_fd(ctx->clients[ci].fd, id, "unable to set hostname");
        else
            (void)send_ok_fd(ctx->clients[ci].fd, id, "{}");
    } else {
        (void)send_err_fd(ctx->clients[ci].fd, id, "unknown command");
    }
}

static void process_client_input(struct daemon_ctx *ctx, int ci)
{
    struct client *c = &ctx->clients[ci];
    for (;;) {
        char *newline;
        size_t consumed;
        if (c->busy)
            return;
        newline = memchr(c->input, '\n', c->input_len);
        if (!newline)
            return;
        consumed = (size_t)(newline - c->input) + 1;
        *newline = '\0';
        dispatch_request(ctx, ci, c->input);
        if (ctx->clients[ci].fd < 0)
            return;
        if (consumed < c->input_len)
            memmove(c->input, c->input + consumed, c->input_len - consumed);
        c->input_len -= consumed;
    }
}

static void read_client(struct daemon_ctx *ctx, int ci)
{
    struct client *c = &ctx->clients[ci];
    for (;;) {
        ssize_t n;
        if (c->input_len >= sizeof(c->input) - 1) {
            (void)send_err_fd(c->fd, 0, "request too large");
            remove_client(ctx, ci);
            return;
        }
        n = recv(c->fd, c->input + c->input_len,
                 sizeof(c->input) - 1 - c->input_len, MSG_DONTWAIT);
        if (n > 0) {
            c->input_len += (size_t)n;
            c->input[c->input_len] = '\0';
            process_client_input(ctx, ci);
            if (ctx->clients[ci].fd < 0 || c->busy)
                return;
            continue;
        }
        if (n == 0) {
            remove_client(ctx, ci);
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        remove_client(ctx, ci);
        return;
    }
}

static void accept_clients(struct daemon_ctx *ctx)
{
    for (;;) {
        int fd = accept(ctx->listen_fd, NULL, NULL);
        int i;
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            return;
        }
        set_cloexec(fd);
        (void)set_nonblock(fd);
        for (i = 0; i < CLIENT_MAX && ctx->clients[i].fd >= 0; ++i)
            ;
        if (i == CLIENT_MAX) {
            (void)send_err_fd(fd, 0, "too many clients");
            close(fd);
        } else {
            ctx->clients[i].fd = fd;
            ctx->clients[i].input_len = 0;
            ctx->clients[i].busy = 0;
            /* Consume an already-queued command before this poll iteration can
             * perform an automatic recovery action.  A nonblocking empty read
             * simply leaves the client for the next iteration. */
            read_client(ctx, i);
        }
    }
}

/* ----- Startup/shutdown ------------------------------------------------- */

static int mkdir_parents(const char *path)
{
    char temp[PATH_MAX];
    char *p;
    if (strlen(path) >= sizeof(temp))
        return -1;
    copy_string(temp, sizeof(temp), path);
    p = strrchr(temp, '/');
    if (!p)
        return 0;
    if (p == temp)
        return 0;
    *p = '\0';
    for (p = temp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(temp, 0755) < 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(temp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int daemonize_process(void)
{
    pid_t pid = fork();
    int fd;
    if (pid < 0)
        return -1;
    if (pid > 0)
        _exit(0);
    if (setsid() < 0)
        return -1;
    if (chdir("/") < 0)
        return -1;
    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        (void)dup2(fd, STDIN_FILENO);
        (void)dup2(fd, STDOUT_FILENO);
        (void)dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }
    return 0;
}

static void usage(const char *name)
{
    fprintf(stderr,
            "usage: %s [--socket PATH] [--wpa-ctrl PATH] [--interface NAME] [--reboot-request PATH] [--reboot-guard PATH] [--foreground] [--verbose] [--debug] [--quiet]\n",
            name);
}

static int parse_args(struct daemon_ctx *ctx, int argc, char **argv)
{
    int i;
    copy_string(ctx->socket_path, sizeof(ctx->socket_path), LE_ADAPTER_NETWORK_SOCK);
    copy_string(ctx->wpa_path, sizeof(ctx->wpa_path),
                "/var/run/wpa_supplicant/wlan0");
    copy_string(ctx->reboot_request_path, sizeof(ctx->reboot_request_path),
                "/tmp/reboot.request");
    copy_string(ctx->reboot_guard_path, sizeof(ctx->reboot_guard_path),
                "/data/libreecho/network-recovery-reboot.guard");
    copy_string(ctx->interface, sizeof(ctx->interface), "wlan0");
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--foreground")) {
            ctx->foreground = 1;
        } else if ((!strcmp(argv[i], "--socket") ||
                    !strcmp(argv[i], "--wpa-ctrl") ||
                    !strcmp(argv[i], "--interface") ||
                    !strcmp(argv[i], "--reboot-request") ||
                    !strcmp(argv[i], "--reboot-guard")) && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(argv[i - 1], "--socket"))
                copy_string(ctx->socket_path, sizeof(ctx->socket_path), value);
            else if (!strcmp(argv[i - 1], "--wpa-ctrl"))
                copy_string(ctx->wpa_path, sizeof(ctx->wpa_path), value);
            else if (!strcmp(argv[i - 1], "--reboot-request"))
                copy_string(ctx->reboot_request_path,
                            sizeof(ctx->reboot_request_path), value);
            else if (!strcmp(argv[i - 1], "--reboot-guard"))
                copy_string(ctx->reboot_guard_path,
                            sizeof(ctx->reboot_guard_path), value);
            else
                copy_string(ctx->interface, sizeof(ctx->interface), value);
        } else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "--debug") ||
                   !strcmp(argv[i], "--quiet") || !strcmp(argv[i], "--syslog")) {
            /* handled by le_log_init */
        } else {
            usage(argv[0]);
            return -1;
        }
    }
    return 0;
}

static void cleanup(struct daemon_ctx *ctx)
{
    int i;
    if (ctx->association.active)
        cancel_association(ctx);
    if (ctx->dhcp.active) {
        (void)kill(ctx->dhcp.pid, SIGTERM);
        (void)waitpid(ctx->dhcp.pid, NULL, 0);
        ctx->dhcp.active = 0;
    }
    for (i = 0; i < CLIENT_MAX; ++i)
        if (ctx->clients[i].fd >= 0)
            close(ctx->clients[i].fd);
    wpa_close(ctx);
    le_gateway_probe_close(&ctx->gateway_probe);
    if (ctx->netlink_fd >= 0)
        close(ctx->netlink_fd);
    if (ctx->listen_fd >= 0)
        close(ctx->listen_fd);
    if (ctx->socket_path[0])
        unlink(ctx->socket_path);
}

int main(int argc, char **argv)
{
    struct daemon_ctx ctx;
    struct sigaction action;
    struct pollfd pfds[4 + CLIENT_MAX];
    int i;

    memset(&ctx, 0, sizeof(ctx));
    ctx.listen_fd = -1;
    ctx.netlink_fd = -1;
    ctx.network_id = -1;
    ctx.wpa.command.fd = -1;
    ctx.wpa.monitor.fd = -1;
    le_gateway_probe_init(&ctx.gateway_probe);
    ctx.dhcp.pid = -1;
    ctx.scan.client_fd = -1;
    ctx.association.client_fd = -1;
    ctx.dhcp.client_fd = -1;
    for (i = 0; i < CLIENT_MAX; ++i)
        ctx.clients[i].fd = -1;
    le_log_init("networkd", argc, argv);
    if (parse_args(&ctx, argc, argv) < 0)
        return 2;
    le_network_health_init(&ctx.health, NULL, monotonic_ms());
    le_log_info("networkd: starting (socket=%s, interface=%s, wpa=%s)",
                ctx.socket_path, ctx.interface, ctx.wpa_path);

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGCHLD, &action, NULL);
    signal(SIGPIPE, SIG_IGN);

    copy_string(ctx.state.interface, sizeof(ctx.state.interface), ctx.interface);
    ctx.state.signal = -1;
    ctx.state.rssi_dbm = -1;
    copy_string(ctx.state.state, sizeof(ctx.state.state), "unavailable");

    if (!ctx.foreground && daemonize_process() < 0)
        return 1;
    if (mkdir_parents(ctx.socket_path) < 0)
        return 1;
    (void)unlink(ctx.socket_path);
    ctx.listen_fd = le_adapter_listen(ctx.socket_path);
    if (ctx.listen_fd < 0)
        return 1;
    (void)chmod(ctx.socket_path, 0660);
    set_cloexec(ctx.listen_fd);
    (void)set_nonblock(ctx.listen_fd);
    ctx.netlink_fd = open_netlink();
    (void)wpa_open(&ctx); /* Missing wpa_supplicant is a runtime state, not fatal. */
    refresh_state(&ctx);

    while (g_running) {
        int nfds = 0;
        int timeout = NETWORKD_POLL_MAX_MS;
        int gateway_fd = ctx.gateway_probe.active ? ctx.gateway_probe.fd : -1;
        long long now = monotonic_ms();
        pfds[nfds].fd = ctx.listen_fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        ++nfds;
        if (ctx.wpa.monitor.fd >= 0) {
            pfds[nfds].fd = ctx.wpa.monitor.fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        if (ctx.netlink_fd >= 0) {
            pfds[nfds].fd = ctx.netlink_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        if (gateway_fd >= 0) {
            pfds[nfds].fd = gateway_fd;
            pfds[nfds].events = POLLIN;
            pfds[nfds].revents = 0;
            ++nfds;
        }
        for (i = 0; i < CLIENT_MAX; ++i) {
            if (ctx.clients[i].fd >= 0) {
                pfds[nfds].fd = ctx.clients[i].fd;
                pfds[nfds].events = POLLIN;
                pfds[nfds].revents = 0;
                ++nfds;
            }
        }
        if (ctx.scan.active && ctx.scan.deadline - now < timeout)
            timeout = (int)(ctx.scan.deadline > now ? ctx.scan.deadline - now : 0);
        if (ctx.association.active && ctx.association.poll_at - now < timeout)
            timeout = (int)(ctx.association.poll_at > now ?
                            ctx.association.poll_at - now : 0);
        if (ctx.dhcp.active && ctx.dhcp.deadline - now < timeout)
            timeout = (int)(ctx.dhcp.deadline > now ? ctx.dhcp.deadline - now : 0);
        if (ctx.gateway_probe.active &&
            ctx.gateway_probe.deadline_ms - now < timeout)
            timeout = (int)(ctx.gateway_probe.deadline_ms > now ?
                            ctx.gateway_probe.deadline_ms - now : 0);
        if (poll(pfds, (nfds_t)nfds, timeout) < 0 && errno != EINTR)
            break;
        now = monotonic_ms();

        if (g_child_event)
            reap_children(&ctx);
        check_dhcp_timeout(&ctx);
        check_scan_timeout(&ctx);
        if (!g_running)
            break;
        if (pfds[0].revents & POLLIN)
            accept_clients(&ctx);
        {
            int pos = 1;
            if (ctx.wpa.monitor.fd >= 0) {
                if (pfds[pos].revents & (POLLIN | POLLERR | POLLHUP))
                    drain_wpa_events(&ctx);
                ++pos;
            }
            if (ctx.netlink_fd >= 0) {
                if (pfds[pos].revents & (POLLIN | POLLERR | POLLHUP))
                    handle_netlink(&ctx);
                ++pos;
            }
            for (i = 0; i < CLIENT_MAX; ++i) {
                int j;
                for (j = 0; j < CLIENT_MAX; ++j)
                    if (j == i && ctx.clients[j].fd >= 0) {
                        /* The client positions are rebuilt each iteration; use
                         * the fd rather than relying on a stale array index. */
                        int k;
                        for (k = pos; k < nfds; ++k)
                            if (pfds[k].fd == ctx.clients[j].fd) {
                                if (pfds[k].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))
                                    read_client(&ctx, j);
                                break;
                            }
                    }
            }
            if (gateway_fd >= 0 && ctx.gateway_probe.active &&
                ctx.gateway_probe.fd == gateway_fd) {
                int gateway_index;
                for (gateway_index = 0; gateway_index < nfds;
                     ++gateway_index)
                    if (pfds[gateway_index].fd == gateway_fd) {
                        if (pfds[gateway_index].revents &
                            (POLLIN | POLLERR | POLLHUP | POLLNVAL))
                            handle_gateway_probe_event(
                                &ctx, pfds[gateway_index].revents, now);
                        break;
                    }
            }
            /* Explicit client commands win over association completion and
             * automatic recovery in this poll iteration. */
            check_association(&ctx, now);
            check_network_health(&ctx, now);
        }
    }
    cleanup(&ctx);
    return 0;
}
