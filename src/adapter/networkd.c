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
#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
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
#define DHCP_TIMEOUT_MS 15000
#define SCAN_TIMEOUT_MS 12000
#define WPA_TIMEOUT_MS 2500

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
    char ssid[128];
    char ip[INET_ADDRSTRLEN];
    char gateway[INET_ADDRSTRLEN];
    char dns[INET_ADDRSTRLEN];
    char mac[18];
    int link_up;
    int signal;
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
};

struct pending_dhcp {
    int active;
    int release;
    pid_t pid;
    int client_fd;
    unsigned long id;
    long long deadline;
};

struct daemon_ctx {
    int listen_fd;
    int netlink_fd;
    char socket_path[PATH_MAX];
    char wpa_path[PATH_MAX];
    char interface[IFNAMSIZ];
    int foreground;
    int network_id;
    struct wpa_pair wpa;
    struct network_state state;
    struct client clients[CLIENT_MAX];
    struct pending_scan scan;
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
    if (!size)
        return;
    if (!src)
        src = "";
    strncpy(dst, src, size - 1);
    dst[size - 1] = '\0';
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
    size_t n = 0;
    if (append_text(out, size, &n, "{\"interface\":") < 0 ||
        append_json_string(out, size, &n, s->interface) < 0 ||
        append_text(out, size, &n, ",\"state\":") < 0 ||
        append_json_string(out, size, &n, s->state) < 0 ||
        append_text(out, size, &n, ",\"ssid\":") < 0 ||
        append_json_string(out, size, &n, s->ssid) < 0 ||
        append_text(out, size, &n, ",\"ip\":") < 0 ||
        append_json_string(out, size, &n, s->ip) < 0 ||
        append_text(out, size, &n, ",\"gateway\":") < 0 ||
        append_json_string(out, size, &n, s->gateway) < 0 ||
        append_text(out, size, &n, ",\"dns\":") < 0 ||
        append_json_string(out, size, &n, s->dns) < 0 ||
        append_text(out, size, &n, ",\"signal\":%d,\"mac\":", s->signal) < 0 ||
        append_json_string(out, size, &n, s->mac) < 0 ||
        append_text(out, size, &n, "}") < 0)
        return -1;
    return (int)n;
}

static int state_equal(const struct network_state *a,
                       const struct network_state *b)
{
    return a->link_up == b->link_up && a->signal == b->signal &&
           !strcmp(a->interface, b->interface) && !strcmp(a->state, b->state) &&
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

    if (ctrl->fd < 0 || !command || !reply || size < 2) {
        errno = EINVAL;
        return -1;
    }
    sent = send(ctrl->fd, command, strlen(command), MSG_NOSIGNAL);
    if (sent < 0 || (size_t)sent != strlen(command))
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
    if (ctx->wpa.command.fd >= 0 && ctx->wpa.monitor.fd >= 0)
        return 0;
    wpa_close(ctx);
    if (wpa_ctrl_open_one(&ctx->wpa.command, ctx->wpa_path, 0) < 0 ||
        wpa_ctrl_open_one(&ctx->wpa.monitor, ctx->wpa_path, 1) < 0 ||
        wpa_ctrl_attach(&ctx->wpa.monitor) < 0) {
        wpa_close(ctx);
        copy_string(ctx->state.state, sizeof(ctx->state.state), "unavailable");
        return -1;
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
    (void)fgets(line, sizeof(line), fp);
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
}

static void refresh_wpa_info(struct daemon_ctx *ctx)
{
    char reply[WPA_REPLY_MAX], value[256];
    int n;

    if (ctx->wpa.command.fd < 0) {
        copy_string(ctx->state.state, sizeof(ctx->state.state), "unavailable");
        return;
    }
    n = wpa_ctrl_request(&ctx->wpa.command, "STATUS\n", reply, sizeof(reply),
                         WPA_TIMEOUT_MS);
    if (n < 0) {
        wpa_close(ctx);
        copy_string(ctx->state.state, sizeof(ctx->state.state), "unavailable");
        return;
    }
    if (wpa_value(reply, "ssid", value, sizeof(value)))
        copy_string(ctx->state.ssid, sizeof(ctx->state.ssid), value);
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
    if (n >= 0 && wpa_value(reply, "RSSI", value, sizeof(value)))
        ctx->state.signal = (int)strtol(value, NULL, 10);
}

static void refresh_state(struct daemon_ctx *ctx)
{
    refresh_interface_info(ctx);
    refresh_wpa_info(ctx);
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

/* ----- DHCP child handling --------------------------------------------- */

static void finish_dhcp(struct daemon_ctx *ctx, int status, int timed_out)
{
    int fd = ctx->dhcp.client_fd;
    unsigned long id = ctx->dhcp.id;
    int release = ctx->dhcp.release;
    char data[LE_ADAPTER_MSG_MAX];
    int success = !timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0;

    ctx->dhcp.active = 0;
    ctx->dhcp.pid = -1;
    if (release) {
        ctx->state.ip[0] = '\0';
        ctx->state.gateway[0] = '\0';
        ctx->state.dns[0] = '\0';
        copy_string(ctx->state.state, sizeof(ctx->state.state), "disconnected");
    } else {
        refresh_interface_info(ctx);
        if (success && ctx->state.ip[0])
            copy_string(ctx->state.state, sizeof(ctx->state.state), "connected");
        else if (!success)
            copy_string(ctx->state.state, sizeof(ctx->state.state), "disconnected");
    }
    if (fd >= 0) {
        int ci = client_index(ctx, fd);
        if (ci >= 0) {
            if (success && (!release && !ctx->state.ip[0]))
                (void)send_err_fd(fd, id, "DHCP completed without an IPv4 address");
            else if (success)
                (void)send_ok_fd(fd, id, state_json(&ctx->state, data, sizeof(data)) >= 0 ? data : "{}");
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
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        if (release)
            execl("/sbin/udhcpc", "udhcpc", "-i", ctx->interface, "-n", "-q",
                  "-R", (char *)NULL);
        else
            execl("/sbin/udhcpc", "udhcpc", "-i", ctx->interface, "-n", "-q",
                  (char *)NULL);
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
        return "wpa3";
    if (strstr(flags, "WPA2"))
        return "wpa2";
    if (strstr(flags, "WPA"))
        return "wpa";
    return "open";
}

static int parse_scan_results(const char *reply, char *data, size_t size)
{
    const char *line = reply;
    size_t used = 0;
    int count = 0;
    if (append_text(data, size, &used, "{\"results\":[") < 0)
        return -1;
    while (*line && count < SCAN_MAX) {
        char row[512], *fields[5], *p;
        char ssid[256], flags[128], signal[32];
        int field = 0;
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
            copy_string(flags, sizeof(flags), fields[3]);
            copy_string(ssid, sizeof(ssid), fields[4]);
            if (count && append_text(data, size, &used, ",") < 0)
                return -1;
            if (append_text(data, size, &used, "{\"ssid\":") < 0 ||
                append_json_string(data, size, &used, ssid) < 0 ||
                append_text(data, size, &used, ",\"security\":") < 0 ||
                append_json_string(data, size, &used, scan_security(flags)) < 0 ||
                append_text(data, size, &used, ",\"signal\":%d}",
                            (int)strtol(signal, NULL, 10)) < 0)
                return -1;
            ++count;
        }
        if (!end)
            break;
        line = end + 1;
    }
    if (append_text(data, size, &used, "]}") < 0)
        return -1;
    return (int)used;
}

static void finish_scan(struct daemon_ctx *ctx, int failed, const char *error)
{
    int fd = ctx->scan.client_fd;
    unsigned long id = ctx->scan.id;
    char reply[WPA_REPLY_MAX], data[LE_ADAPTER_MSG_MAX];
    int ci;

    ctx->scan.active = 0;
    ctx->scan.client_fd = -1;
    if (fd < 0)
        return;
    ci = client_index(ctx, fd);
    if (ci < 0)
        return;
    if (failed)
        (void)send_err_fd(fd, id, error ? error : "scan failed");
    else if (wpa_call(ctx, "GET_SCAN_RESULTS\n", reply, sizeof(reply)) < 0 ||
             parse_scan_results(reply, data, sizeof(data)) < 0)
        (void)send_err_fd(fd, id, "unable to read scan results");
    else
        (void)send_ok_fd(fd, id, data);
    ctx->clients[ci].busy = 0;
}

static void check_scan_timeout(struct daemon_ctx *ctx)
{
    if (ctx->scan.active && monotonic_ms() >= ctx->scan.deadline)
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

static int connect_network(struct daemon_ctx *ctx, const char *ssid,
                           const char *psk, const char *security)
{
    char reply[WPA_REPLY_MAX], quoted[512], command[768];
    int id;
    const char *key_mgmt;
    if (wpa_call(ctx, "ADD_NETWORK\n", reply, sizeof(reply)) < 0)
        return -1;
    id = (int)strtol(reply, NULL, 10);
    if (id < 0 || (reply[0] < '0' || reply[0] > '9'))
        return -1;
    if (wpa_quote(quoted, sizeof(quoted), ssid) < 0 ||
        snprintf(command, sizeof(command), "SET_NETWORK %d ssid %s\n", id,
                 quoted) >= (int)sizeof(command) ||
        wpa_ok(ctx, command, reply, sizeof(reply)) < 0)
        return -1;

    if (!security || !strcmp(security, "open") || !strcmp(security, "none"))
        key_mgmt = "NONE";
    else if (strstr(security, "wpa3") || strstr(security, "SAE"))
        key_mgmt = "SAE";
    else
        key_mgmt = "WPA-PSK";
    if (snprintf(command, sizeof(command), "SET_NETWORK %d key_mgmt %s\n", id,
                 key_mgmt) >= (int)sizeof(command) ||
        wpa_ok(ctx, command, reply, sizeof(reply)) < 0)
        return -1;
    if (strcmp(key_mgmt, "NONE")) {
        if (!psk || !*psk || wpa_quote(quoted, sizeof(quoted), psk) < 0 ||
            snprintf(command, sizeof(command), "SET_NETWORK %d psk %s\n", id,
                     quoted) >= (int)sizeof(command) ||
            wpa_ok(ctx, command, reply, sizeof(reply)) < 0)
            return -1;
    }
    if (snprintf(command, sizeof(command), "ENABLE_NETWORK %d\n", id) >=
            (int)sizeof(command) || wpa_ok(ctx, command, reply, sizeof(reply)) < 0 ||
        snprintf(command, sizeof(command), "SELECT_NETWORK %d\n", id) >=
            (int)sizeof(command) || wpa_ok(ctx, command, reply, sizeof(reply)) < 0)
        return -1;
    ctx->network_id = id;
    return 0;
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
        if (ctx->scan.active) {
            (void)send_err_fd(ctx->clients[ci].fd, id, "scan already in progress");
            return;
        }
        le_log_info("networkd: scan requested");
        if (wpa_ok(ctx, "SCAN\n", reply, sizeof(reply)) < 0) {
            le_log_error("networkd: wpa_supplicant scan unavailable");
            (void)send_err_fd(ctx->clients[ci].fd, id, "wpa_supplicant scan unavailable");
            return;
        }
        ctx->scan.active = 1;
        ctx->scan.client_fd = ctx->clients[ci].fd;
        ctx->scan.id = id;
        ctx->scan.deadline = monotonic_ms() + SCAN_TIMEOUT_MS;
        ctx->clients[ci].busy = 1;
    } else if (!strcmp(cmd, "connect")) {
        char ssid[256], psk[256], security[64];
        int have_ssid = json_string_arg(args, "ssid", ssid, sizeof(ssid));
        int have_psk = json_string_arg(args, "psk", psk, sizeof(psk));
        int have_security = json_string_arg(args, "security", security, sizeof(security));
        if (have_ssid != 1 || have_psk < 0 || have_security < 0) {
            (void)send_err_fd(ctx->clients[ci].fd, id, "connect requires ssid, psk, security");
            return;
        }
        if (ctx->dhcp.active) {
            (void)send_err_fd(ctx->clients[ci].fd, id, "DHCP already in progress");
            return;
        }
        le_log_info("networkd: connect to ssid=\"%s\" security=%s", ssid, have_security == 1 ? security : "wpa2");
        if (connect_network(ctx, ssid, have_psk == 1 ? psk : "",
                            have_security == 1 ? security : "wpa2") < 0) {
            le_log_error("networkd: wpa_supplicant rejected network \"%s\"", ssid);
            (void)send_err_fd(ctx->clients[ci].fd, id, "wpa_supplicant rejected network");
            return;
        }
        copy_string(ctx->state.ssid, sizeof(ctx->state.ssid), ssid);
        copy_string(ctx->state.state, sizeof(ctx->state.state), "connecting");
        ctx->clients[ci].busy = 1;
        if (start_dhcp(ctx, 0, ctx->clients[ci].fd, id) < 0) {
            ctx->clients[ci].busy = 0;
            (void)send_err_fd(ctx->clients[ci].fd, id, "unable to start DHCP");
        }
    } else if (!strcmp(cmd, "disconnect")) {
        char reply[WPA_REPLY_MAX];
        le_log_info("networkd: disconnect requested");
        if (ctx->dhcp.active)
            cancel_dhcp(ctx);
        if (ctx->wpa.command.fd >= 0)
            (void)wpa_ok(ctx, "DISABLE_NETWORK all\n", reply, sizeof(reply));
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
            "usage: %s [--socket PATH] [--wpa-ctrl PATH] [--interface NAME] [--foreground] [--verbose] [--debug] [--quiet]\n",
            name);
}

static int parse_args(struct daemon_ctx *ctx, int argc, char **argv)
{
    int i;
    copy_string(ctx->socket_path, sizeof(ctx->socket_path), LE_ADAPTER_NETWORK_SOCK);
    copy_string(ctx->wpa_path, sizeof(ctx->wpa_path),
                "/var/run/wpa_supplicant/wlan0");
    copy_string(ctx->interface, sizeof(ctx->interface), "wlan0");
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--foreground")) {
            ctx->foreground = 1;
        } else if ((!strcmp(argv[i], "--socket") ||
                    !strcmp(argv[i], "--wpa-ctrl") ||
                    !strcmp(argv[i], "--interface")) && i + 1 < argc) {
            const char *value = argv[++i];
            if (!strcmp(argv[i - 1], "--socket"))
                copy_string(ctx->socket_path, sizeof(ctx->socket_path), value);
            else if (!strcmp(argv[i - 1], "--wpa-ctrl"))
                copy_string(ctx->wpa_path, sizeof(ctx->wpa_path), value);
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
    if (ctx->dhcp.active) {
        (void)kill(ctx->dhcp.pid, SIGTERM);
        (void)waitpid(ctx->dhcp.pid, NULL, 0);
        ctx->dhcp.active = 0;
    }
    for (i = 0; i < CLIENT_MAX; ++i)
        if (ctx->clients[i].fd >= 0)
            close(ctx->clients[i].fd);
    wpa_close(ctx);
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
    struct pollfd pfds[3 + CLIENT_MAX];
    int i;

    memset(&ctx, 0, sizeof(ctx));
    ctx.listen_fd = -1;
    ctx.netlink_fd = -1;
    ctx.network_id = -1;
    ctx.wpa.command.fd = -1;
    ctx.wpa.monitor.fd = -1;
    ctx.dhcp.pid = -1;
    ctx.scan.client_fd = -1;
    ctx.dhcp.client_fd = -1;
    for (i = 0; i < CLIENT_MAX; ++i)
        ctx.clients[i].fd = -1;
    le_log_init("networkd", argc, argv);
    if (parse_args(&ctx, argc, argv) < 0)
        return 2;
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
        int timeout = 1000;
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
        if (ctx.dhcp.active && ctx.dhcp.deadline - now < timeout)
            timeout = (int)(ctx.dhcp.deadline > now ? ctx.dhcp.deadline - now : 0);
        if (poll(pfds, (nfds_t)nfds, timeout) < 0 && errno != EINTR)
            break;

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
        }
    }
    cleanup(&ctx);
    return 0;
}
