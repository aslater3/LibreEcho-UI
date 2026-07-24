/* LibreEcho AirPlay 2 integration controller.
 *
 * The controller is started with the other local daemons, but AirPlay itself
 * is deliberately stopped until the user enables the integration.  NQPTP is
 * started before Shairport Sync because AirPlay 2 uses it for PTP timing.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "adapter.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define INPUT_MAX LE_ADAPTER_MSG_MAX

struct airplay_ctx {
    int listener;
    char socket_path[128];
    char runtime_root[128];
    char nqptp_path[128];
    char shairport_path[128];
    char avahi_path[128];
    char dbus_path[128];
    char audio_path[128];
    char config_path[128];
    pid_t dbus_pid;
    pid_t avahi_pid;
    pid_t nqptp_pid;
    pid_t audio_pid;
    pid_t shairport_pid;
    int enabled;
};

static volatile sig_atomic_t running = 1;

#define AIRPLAY_CHROOT "/bin/busybox"

static void on_signal(int signo)
{
    if (signo == SIGTERM || signo == SIGINT)
        running = 0;
}

static int json_bool(const char *json, const char *key, int *value)
{
    char needle[64];
    const char *p;
    int n;

    n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return -1;
    p = strstr(json, needle);
    if (!p)
        return -1;
    p = strchr(p + n, ':');
    if (!p)
        return -1;
    ++p;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        ++p;
    if (!strncmp(p, "true", 4)) {
        *value = 1;
        return 0;
    }
    if (!strncmp(p, "false", 5)) {
        *value = 0;
        return 0;
    }
    return -1;
}

static int child_alive(pid_t pid)
{
    return pid > 0 && kill(pid, 0) == 0;
}

static int child_running(pid_t *pid)
{
    int status;
    pid_t result;

    if (!pid || *pid <= 0)
        return 0;
    result = waitpid(*pid, &status, WNOHANG);
    if (result == *pid) {
        *pid = -1;
        return 0;
    }
    return result < 0 ? 0 : child_alive(*pid);
}

static int wait_for_runtime_file(const struct airplay_ctx *ctx,
                                 const char *relative, int attempts)
{
    char path[256];
    int i;

    if (!ctx || !relative || attempts < 1)
        return 0;
    if (snprintf(path, sizeof(path), "%s%s", ctx->runtime_root, relative) < 0)
        return 0;
    for (i = 0; i < attempts; ++i) {
        if (access(path, F_OK) == 0)
            return 1;
        usleep(100000);
    }
    return 0;
}

static int reap(struct airplay_ctx *ctx)
{
    int status;
    pid_t pid;
    int lost = 0;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (pid == ctx->dbus_pid) {
            ctx->dbus_pid = -1;
            lost = 1;
        }
        if (pid == ctx->avahi_pid) {
            ctx->avahi_pid = -1;
            lost = 1;
        }
        if (pid == ctx->nqptp_pid) {
            ctx->nqptp_pid = -1;
            lost = 1;
        }
        if (pid == ctx->audio_pid) {
            ctx->audio_pid = -1;
            lost = 1;
        }
        if (pid == ctx->shairport_pid) {
            ctx->shairport_pid = -1;
            lost = 1;
        }
    }
    return lost;
}

static void stop_child(pid_t *pid)
{
    int status;
    int i;

    if (!pid || *pid <= 0)
        return;
    (void)kill(*pid, SIGTERM);
    for (i = 0; i < 20; ++i) {
        if (waitpid(*pid, &status, WNOHANG) == *pid) {
            *pid = -1;
            return;
        }
        usleep(100000);
    }
    (void)kill(*pid, SIGKILL);
    (void)waitpid(*pid, &status, 0);
    *pid = -1;
}

static pid_t spawn_nqptp(const struct airplay_ctx *ctx)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    execl(AIRPLAY_CHROOT, AIRPLAY_CHROOT, "chroot", ctx->runtime_root,
          ctx->nqptp_path, (char *)NULL);
    _exit(127);
}

static pid_t spawn_dbus(const struct airplay_ctx *ctx)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    execl(AIRPLAY_CHROOT, AIRPLAY_CHROOT, "chroot", ctx->runtime_root,
          ctx->dbus_path, "--nofork", "--nopidfile",
          "--config-file=/etc/dbus-1/system.conf", (char *)NULL);
    _exit(127);
}

static pid_t spawn_avahi(const struct airplay_ctx *ctx)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    execl(AIRPLAY_CHROOT, AIRPLAY_CHROOT, "chroot", ctx->runtime_root,
          ctx->avahi_path, "--no-chroot", "--no-drop-root",
          "--no-rlimits", (char *)NULL);
    _exit(127);
}

static pid_t spawn_shairport(const struct airplay_ctx *ctx)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    execl(AIRPLAY_CHROOT, AIRPLAY_CHROOT, "chroot", ctx->runtime_root,
          ctx->shairport_path,
          "-vvv", "--configfile", ctx->config_path, (char *)NULL);
    _exit(127);
}

static pid_t spawn_audio(const struct airplay_ctx *ctx)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    execl(AIRPLAY_CHROOT, AIRPLAY_CHROOT, "chroot", ctx->runtime_root,
          ctx->audio_path, (char *)NULL);
    _exit(127);
}

static int set_enabled(struct airplay_ctx *ctx, int enabled)
{
    int i;
    if (enabled == ctx->enabled && (!enabled ||
                                    (child_alive(ctx->dbus_pid) &&
                                     child_alive(ctx->avahi_pid) &&
                                     child_alive(ctx->nqptp_pid) &&
                                     child_alive(ctx->audio_pid) &&
                                     child_alive(ctx->shairport_pid))))
        return 0;
    if (!enabled) {
        stop_child(&ctx->shairport_pid);
        stop_child(&ctx->audio_pid);
        stop_child(&ctx->nqptp_pid);
        stop_child(&ctx->avahi_pid);
        stop_child(&ctx->dbus_pid);
        ctx->enabled = 0;
        le_log_info("airplayd: AirPlay 2 disabled");
        return 0;
    }
    char path[256];
#define RUNTIME_ACCESS(relative, mode) \
    (snprintf(path, sizeof(path), "%s%s", ctx->runtime_root, (relative)), \
     access(path, (mode)) < 0)
    if (access(AIRPLAY_CHROOT, X_OK) < 0 ||
        RUNTIME_ACCESS(ctx->dbus_path, X_OK) ||
        RUNTIME_ACCESS(ctx->avahi_path, X_OK) ||
        RUNTIME_ACCESS(ctx->nqptp_path, X_OK) ||
        RUNTIME_ACCESS(ctx->audio_path, X_OK) ||
        RUNTIME_ACCESS(ctx->shairport_path, X_OK) ||
        RUNTIME_ACCESS(ctx->config_path, R_OK)) {
#undef RUNTIME_ACCESS
        return -1;
    }
#undef RUNTIME_ACCESS
    ctx->dbus_pid = spawn_dbus(ctx);
    if (ctx->dbus_pid < 0)
        return -1;
    if (!wait_for_runtime_file(ctx, "/run/dbus/system_bus_socket", 30))
        goto fail;
    ctx->avahi_pid = spawn_avahi(ctx);
    if (ctx->avahi_pid < 0) {
        stop_child(&ctx->dbus_pid);
        return -1;
    }
    for (i = 0; i < 10 && child_running(&ctx->avahi_pid); ++i)
        usleep(100000);
    if (ctx->avahi_pid <= 0)
        goto fail;
    ctx->nqptp_pid = spawn_nqptp(ctx);
    if (ctx->nqptp_pid < 0)
        goto fail;
    if (!wait_for_runtime_file(ctx, "/dev/shm/nqptp", 30))
        goto fail;
    ctx->audio_pid = spawn_audio(ctx);
    if (ctx->audio_pid < 0)
        goto fail;
    ctx->shairport_pid = spawn_shairport(ctx);
    if (ctx->shairport_pid < 0)
        goto fail;
    /* A successful fork is not a successful enable: a child may reject its
     * configuration or a required runtime mount may still be absent. */
    for (i = 0; i < 20; ++i) {
        if (!child_running(&ctx->dbus_pid) || !child_running(&ctx->avahi_pid) ||
            !child_running(&ctx->nqptp_pid) || !child_running(&ctx->audio_pid) ||
            !child_running(&ctx->shairport_pid))
            goto fail;
        usleep(100000);
    }
    ctx->enabled = 1;
    le_log_info("airplayd: AirPlay 2 enabled (D-Bus, Avahi, NQPTP, Shairport Sync)");
    return 0;
fail:
    stop_child(&ctx->shairport_pid);
    stop_child(&ctx->audio_pid);
    stop_child(&ctx->nqptp_pid);
    stop_child(&ctx->avahi_pid);
    stop_child(&ctx->dbus_pid);
    return -1;
}

static int request(struct airplay_ctx *ctx, char *message,
                   char *response, size_t response_size)
{
    char command[64];
    char *args;
    unsigned long id;
    int enabled;

    if (le_adapter_parse_request(message, command, sizeof(command), &args, &id) < 0)
        return le_adapter_respond_err(response, response_size, 0, "malformed request");
    if (!strcmp(command, "status")) {
        char data[256];
        char path[256];
        int available = access(AIRPLAY_CHROOT, X_OK) == 0;
#define RUNTIME_AVAILABLE(relative, mode) \
        (snprintf(path, sizeof(path), "%s%s", ctx->runtime_root, (relative)), \
         access(path, (mode)) == 0)
        available = available && RUNTIME_AVAILABLE(ctx->dbus_path, X_OK) &&
                    RUNTIME_AVAILABLE(ctx->avahi_path, X_OK) &&
                    RUNTIME_AVAILABLE(ctx->nqptp_path, X_OK) &&
                    RUNTIME_AVAILABLE(ctx->audio_path, X_OK) &&
                    RUNTIME_AVAILABLE(ctx->shairport_path, X_OK) &&
                    RUNTIME_AVAILABLE(ctx->config_path, R_OK);
#undef RUNTIME_AVAILABLE
        snprintf(data, sizeof(data),
                 "{\"available\":%s,\"enabled\":%s,\"dbus_running\":%s,\"avahi_running\":%s,\"nqptp_running\":%s,\"audio_running\":%s,\"shairport_running\":%s}",
                 available ? "true" : "false", ctx->enabled ? "true" : "false",
                 child_alive(ctx->dbus_pid) ? "true" : "false",
                 child_alive(ctx->avahi_pid) ? "true" : "false",
                 child_alive(ctx->nqptp_pid) ? "true" : "false",
                 child_alive(ctx->audio_pid) ? "true" : "false",
                 child_alive(ctx->shairport_pid) ? "true" : "false");
        return le_adapter_respond_ok(response, response_size, id, data);
    }
    if (!strcmp(command, "set_enabled")) {
        if (json_bool(args, "enabled", &enabled) < 0)
            return le_adapter_respond_err(response, response_size, id,
                                          "enabled must be boolean");
        if (set_enabled(ctx, enabled) < 0)
            return le_adapter_respond_err(response, response_size, id,
                                          "AirPlay 2 binaries or configuration are unavailable");
        return le_adapter_respond_ok(response, response_size, id, "{}");
    }
    return le_adapter_respond_err(response, response_size, id, "unknown command");
}

static int send_response(int fd, const char *response, int length)
{
    size_t sent = 0;
    while (sent < (size_t)length) {
        ssize_t n = write(fd, response + sent, (size_t)length - sent);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct airplay_ctx ctx;
    struct sigaction action;
    int foreground = 0;
    int i;

    memset(&ctx, 0, sizeof(ctx));
    ctx.listener = -1;
    snprintf(ctx.runtime_root, sizeof(ctx.runtime_root),
             "/run/libreecho/features/airplay2/root");
    ctx.dbus_pid = -1;
    ctx.avahi_pid = -1;
    ctx.nqptp_pid = -1;
    ctx.audio_pid = -1;
    ctx.shairport_pid = -1;
    snprintf(ctx.socket_path, sizeof(ctx.socket_path), "%s", LE_ADAPTER_AIRPLAY_SOCK);
    snprintf(ctx.nqptp_path, sizeof(ctx.nqptp_path), "/usr/local/sbin/nqptp");
    snprintf(ctx.shairport_path, sizeof(ctx.shairport_path), "/usr/local/sbin/shairport-sync");
    snprintf(ctx.avahi_path, sizeof(ctx.avahi_path), "/usr/local/sbin/avahi-daemon");
    snprintf(ctx.dbus_path, sizeof(ctx.dbus_path), "/usr/local/sbin/dbus-daemon");
    snprintf(ctx.audio_path, sizeof(ctx.audio_path), "/usr/local/sbin/libreecho-airplay-audio");
    snprintf(ctx.config_path, sizeof(ctx.config_path), "/etc/libreecho/airplay2.conf");
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--foreground")) foreground = 1;
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc) snprintf(ctx.socket_path, sizeof(ctx.socket_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--root") && i + 1 < argc) snprintf(ctx.runtime_root, sizeof(ctx.runtime_root), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--nqptp") && i + 1 < argc) snprintf(ctx.nqptp_path, sizeof(ctx.nqptp_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--shairport-sync") && i + 1 < argc) snprintf(ctx.shairport_path, sizeof(ctx.shairport_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) snprintf(ctx.audio_path, sizeof(ctx.audio_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) snprintf(ctx.config_path, sizeof(ctx.config_path), "%s", argv[++i]);
        else { fprintf(stderr, "Usage: %s [--foreground] [--socket PATH] [--root PATH] [--nqptp PATH] [--shairport-sync PATH] [--config PATH]\n", argv[0]); return 1; }
    }
    (void)foreground;
    le_log_init("airplayd", argc, argv);
    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    (void)sigaction(SIGTERM, &action, NULL);
    (void)sigaction(SIGINT, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    ctx.listener = le_adapter_listen(ctx.socket_path);
    if (ctx.listener < 0) {
        perror("airplayd: listen");
        return 1;
    }
    le_log_info("airplayd: starting (socket=%s, disabled by default)", ctx.socket_path);
    while (running) {
        struct pollfd pfd = { ctx.listener, POLLIN, 0 };
        int client;
        if (reap(&ctx) && ctx.enabled) {
            /* A child can exit after enable succeeded, most notably when an
             * ALSA stream is opened.  Clear runtime state and reap the
             * remaining children so the next enable starts cleanly instead
             * of accumulating orphaned D-Bus/Avahi instances. */
            ctx.enabled = 0;
            le_log_warn("airplayd: AirPlay child exited; stopping remaining children");
            stop_child(&ctx.shairport_pid);
            stop_child(&ctx.audio_pid);
            stop_child(&ctx.nqptp_pid);
            stop_child(&ctx.avahi_pid);
            stop_child(&ctx.dbus_pid);
        }
        if (poll(&pfd, 1, 500) < 0 && errno != EINTR)
            break;
        if (!(pfd.revents & POLLIN))
            continue;
        client = le_adapter_accept(ctx.listener);
        if (client >= 0) {
            char message[INPUT_MAX], response[INPUT_MAX];
            ssize_t n = read(client, message, sizeof(message) - 1);
            if (n > 0) {
                int length;
                message[n] = '\0';
                length = request(&ctx, message, response, sizeof(response));
                if (length > 0) (void)send_response(client, response, length);
            }
            close(client);
        }
    }
    set_enabled(&ctx, 0);
    close(ctx.listener);
    unlink(ctx.socket_path);
    return 0;
}
