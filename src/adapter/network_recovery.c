/*
 * Secure recovery access-point core (issue #96).
 *
 * See network_recovery.h for the contract.  This file deliberately contains
 * no HTTP/auth logic: it validates the boot marker, probes packaged
 * capabilities, manages the softAP children with exact pid ownership, and
 * serializes bounded status.  The provisioning secret never leaves this unit
 * except through le_recovery_secret_json(), which the caller must gate on an
 * authenticated owner request.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "network_recovery.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#define RECOVERY_SETTLE_MS 400LL

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
        } else if (*p >= 32) {
            if (append_text(dst, size, used, "%c", *p) < 0)
                return -1;
        }
        ++p;
    }
    return append_text(dst, size, used, "\"");
}

/* ----- Command oracles (real backend) ---------------------------------- */

static int real_available(void *ctx, const char *path)
{
    struct stat status;

    (void)ctx;
    if (!path || !path[0])
        return 0;
    if (stat(path, &status) < 0)
        return 0;
    return S_ISREG(status.st_mode) && access(path, X_OK) == 0;
}

static unsigned long long read_start_token(pid_t pid)
{
    char path[64], buffer[1024], *closing, *cursor;
    int fd, n, field;

    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >=
        (int)sizeof(path))
        return 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    n = (int)read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buffer[n] = '\0';
    closing = strrchr(buffer, ')');
    if (!closing || !closing[1])
        return 0;
    cursor = closing + 1;
    /* Field 3 (state) follows the comm field; starttime is field 22. */
    for (field = 3; field <= 22; ++field) {
        while (*cursor == ' ')
            ++cursor;
        if (*cursor == '\0')
            return 0;
        if (field == 22)
            return strtoull(cursor, NULL, 10);
        while (*cursor && *cursor != ' ')
            ++cursor;
    }
    return 0;
}

static unsigned long long real_identity(void *ctx, pid_t pid)
{
    (void)ctx;
    return read_start_token(pid);
}

static pid_t real_spawn(void *ctx, char *const argv[], char *err, size_t err_size)
{
    pid_t pid;

    (void)ctx;
    pid = fork();
    if (pid < 0) {
        copy_string(err, err_size, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDIN_FILENO);
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

static int real_signal(void *ctx, pid_t pid, int sig)
{
    (void)ctx;
    if (pid <= 0)
        return -1;
    return kill(pid, sig);
}

static int real_wait(void *ctx, pid_t pid, int *status)
{
    pid_t result;

    (void)ctx;
    result = waitpid(pid, status, WNOHANG);
    if (result == 0)
        return 1;   /* still alive */
    if (result < 0)
        return errno == ECHILD ? 0 : -1;
    return 0;       /* reaped */
}

static int real_probe(void *ctx, char *const argv[])
{
    pid_t pid;
    int status = 0;

    (void)ctx;
    pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            (void)dup2(devnull, STDOUT_FILENO);
            (void)dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO)
                close(devnull);
        }
        execv(argv[0], argv);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

struct le_recovery_backend le_recovery_backend_real(void)
{
    struct le_recovery_backend backend;

    memset(&backend, 0, sizeof(backend));
    backend.spawn = real_spawn;
    backend.signal_child = real_signal;
    backend.wait_child = real_wait;
    backend.probe = real_probe;
    backend.available = real_available;
    backend.identity = real_identity;
    return backend;
}

/* ----- Config ----------------------------------------------------------- */

void le_recovery_config_default(struct le_recovery_config *config,
                                const char *interface)
{
    memset(config, 0, sizeof(*config));
    copy_string(config->marker_path, sizeof(config->marker_path),
                "/run/libreecho/recovery-mode");
    copy_string(config->psk_path, sizeof(config->psk_path),
                LE_RECOVERY_PSK_PATH_DEFAULT);
    copy_string(config->config_path, sizeof(config->config_path),
                LE_RECOVERY_CONFIG_PATH_DEFAULT);
    copy_string(config->run_dir, sizeof(config->run_dir), "/run/libreecho");
    copy_string(config->interface, sizeof(config->interface),
                interface && interface[0] ? interface : "wlan0");
    copy_string(config->serial_path, sizeof(config->serial_path),
                "/proc/idme/serial");
    copy_string(config->hostapd_bin, sizeof(config->hostapd_bin),
                "/usr/local/sbin/hostapd");
    copy_string(config->hostapd_conf, sizeof(config->hostapd_conf),
                "/run/libreecho/hostapd.conf");
    copy_string(config->dhcp_bin, sizeof(config->dhcp_bin),
                "/usr/local/sbin/dnsmasq");
    copy_string(config->dhcp_conf, sizeof(config->dhcp_conf),
                "/run/libreecho/recovery-dnsmasq.conf");
    copy_string(config->led_socket, sizeof(config->led_socket),
                "/run/libreecho/led.sock");
    /* Platform helpers: the radio/address step hostapd and dnsmasq depend on. */
    copy_string(config->net_up_cmd, sizeof(config->net_up_cmd),
                "/usr/local/sbin/libreecho-recovery-net-up");
    copy_string(config->net_down_cmd, sizeof(config->net_down_cmd),
                "/usr/local/sbin/libreecho-recovery-net-down");
    copy_string(config->ap_address, sizeof(config->ap_address),
                LE_RECOVERY_AP_ADDRESS_DEFAULT);
    config->auto_timeout_ms = LE_RECOVERY_TIMEOUT_DEFAULT_MS;
    config->start_timeout_ms = LE_RECOVERY_START_DEFAULT_MS;
    config->stop_timeout_ms = LE_RECOVERY_STOP_DEFAULT_MS;
    config->enabled = 1;        /* physical recovery is on by default */
    config->auto_enabled = 0;   /* explicit owner opt-in only */
    config->rate_window_ms = LE_RECOVERY_RATE_WINDOW_DEFAULT_MS;
    config->rate_max = LE_RECOVERY_RATE_MAX_DEFAULT;
    config->require_tmpfs = 1;
    config->require_root_owner = 1;
    config->require_root_peer = 1;
}

void le_recovery_config_bound(struct le_recovery_config *config)
{
#ifdef LE_NETWORKD_TESTING
    /* Host fixtures need short windows; production keeps the bounded floor so
     * a configured value can never open an unbounded unattended AP. */
    (void)LE_RECOVERY_TIMEOUT_MIN_MS;
    (void)LE_RECOVERY_TIMEOUT_MAX_MS;
#else
    if (config->auto_timeout_ms < LE_RECOVERY_TIMEOUT_MIN_MS ||
        config->auto_timeout_ms > LE_RECOVERY_TIMEOUT_MAX_MS)
        config->auto_timeout_ms = LE_RECOVERY_TIMEOUT_DEFAULT_MS;
#endif
    if (config->start_timeout_ms <= 0 || config->start_timeout_ms > 60000)
        config->start_timeout_ms = LE_RECOVERY_START_DEFAULT_MS;
    if (config->stop_timeout_ms <= 0 || config->stop_timeout_ms > 60000)
        config->stop_timeout_ms = LE_RECOVERY_STOP_DEFAULT_MS;
    if (config->rate_window_ms <= 0)
        config->rate_window_ms = LE_RECOVERY_RATE_WINDOW_DEFAULT_MS;
    if (config->rate_max <= 0)
        config->rate_max = LE_RECOVERY_RATE_MAX_DEFAULT;
}

void le_recovery_init(struct le_recovery *recovery,
                      const struct le_recovery_config *config,
                      const struct le_recovery_backend *backend,
                      long long now_ms)
{
    memset(recovery, 0, sizeof(*recovery));
    recovery->config = *config;
    le_recovery_config_bound(&recovery->config);
    recovery->backend = backend ? *backend : le_recovery_backend_real();
    recovery->mode = LE_RECOVERY_MODE_CLIENT;
    recovery->trigger = LE_RECOVERY_TRIGGER_NONE;
    recovery->last_activity_ms = now_ms;
    recovery->secret_available = 0;
}

/* ----- Marker validation ------------------------------------------------ */

static int read_marker_content(const char *path, char *out, size_t out_size,
                               char *reason, size_t reason_size)
{
    char buffer[LE_RECOVERY_MARKER_MAX];
    ssize_t n;
    int fd;

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            copy_string(reason, reason_size, "no-marker");
        else if (errno == ELOOP)
            copy_string(reason, reason_size, "marker-symlink");
        else
            copy_string(reason, reason_size, "marker-open-failed");
        return -1;
    }
    n = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (n <= 0) {
        copy_string(reason, reason_size, "marker-empty");
        return -1;
    }
    buffer[n] = '\0';
    if (strncmp(buffer, LE_RECOVERY_MARKER_TAG,
                strlen(LE_RECOVERY_MARKER_TAG)) != 0) {
        copy_string(reason, reason_size, "marker-content");
        return -1;
    }
    copy_string(out, out_size, buffer);
    return 0;
}

int le_recovery_marker_check(const struct le_recovery_config *config,
                             char *reason, size_t reason_size)
{
    struct stat status;
    struct statfs filesystem;
    char content[LE_RECOVERY_MARKER_MAX];
    int fd;

    if (reason && reason_size)
        reason[0] = '\0';
    if (!config || !config->marker_path[0]) {
        copy_string(reason, reason_size, "no-marker-path");
        return 0;
    }
    fd = open(config->marker_path,
              O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) {
            copy_string(reason, reason_size, "no-marker");
            return 0;   /* normal boot */
        }
        if (errno == ELOOP) {
            copy_string(reason, reason_size, "marker-symlink");
            return -1;
        }
        copy_string(reason, reason_size, "marker-open-failed");
        return -1;
    }
    if (fstat(fd, &status) < 0) {
        close(fd);
        copy_string(reason, reason_size, "marker-stat-failed");
        return -1;
    }
    close(fd);
    if (!S_ISREG(status.st_mode)) {
        copy_string(reason, reason_size, "marker-not-regular");
        return -1;
    }
    if (status.st_mode & 022) {
        copy_string(reason, reason_size, "marker-world-accessible");
        return -1;
    }
    if (config->require_root_owner && status.st_uid != 0) {
        copy_string(reason, reason_size, "marker-not-root-owned");
        return -1;
    }
    if (config->require_tmpfs) {
        if (statfs(config->marker_path, &filesystem) < 0) {
            copy_string(reason, reason_size, "marker-statfs-failed");
            return -1;
        }
        if ((unsigned long)filesystem.f_type != (unsigned long)TMPFS_MAGIC) {
            copy_string(reason, reason_size, "marker-not-tmpfs");
            return -1;
        }
    }
    if (read_marker_content(config->marker_path, content, sizeof(content),
                            reason, reason_size) < 0)
        return -1;
    copy_string(reason, reason_size, "physical-hold");
    return 1;
}

/* ----- Provisioning secret --------------------------------------------- */

static int random_bytes(unsigned char *out, size_t length)
{
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t offset = 0;

    if (fd < 0)
        return -1;
    while (offset < length) {
        ssize_t n = read(fd, out + offset, length - offset);
        if (n > 0) {
            offset += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

static int generate_psk(char *out, size_t out_size)
{
    static const char alphabet[] =
        "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    unsigned char raw[32];
    size_t i;

    if (out_size < 33)
        return -1;
    if (random_bytes(raw, sizeof(raw)) < 0)
        return -1;
    for (i = 0; i < sizeof(raw); ++i)
        out[i] = alphabet[raw[i] % (sizeof(alphabet) - 1)];
    out[sizeof(raw)] = '\0';
    return 0;
}

static int ensure_parent_dir(const char *path)
{
    char temp[PATH_MAX];
    char *slash;

    if (!path || strlen(path) >= sizeof(temp))
        return -1;
    copy_string(temp, sizeof(temp), path);
    slash = strrchr(temp, '/');
    if (!slash || slash == temp)
        return 0;
    *slash = '\0';
    if (mkdir(temp, 0700) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

/*
 * Create and validate the directory that owns a persistent secret/config file.
 * Every missing component is created root-only (0700); no component may be a
 * symlink, and the immediate parent must be a real directory that is neither
 * group- nor world-accessible (and root-owned in production).  This is the
 * "protected parent" the persisted recovery password and configuration live
 * under, so a pre-planted writable directory can never redirect or expose them.
 */
static int ensure_secure_parent_dir(const struct le_recovery_config *config,
                                    const char *path, char *reason,
                                    size_t reason_size)
{
    char parent[PATH_MAX];
    char *slash, *cursor;
    struct stat status;

    if (!path || !path[0] || strlen(path) >= sizeof(parent)) {
        copy_string(reason, reason_size, "psk-path-too-long");
        return -1;
    }
    copy_string(parent, sizeof(parent), path);
    slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        return 0;   /* no parent component to guard */
    *slash = '\0';
    for (cursor = parent + 1; *cursor; ++cursor) {
        if (*cursor != '/')
            continue;
        *cursor = '\0';
        if (lstat(parent, &status) < 0) {
            if (errno != ENOENT ||
                (mkdir(parent, 0700) < 0 && errno != EEXIST)) {
                copy_string(reason, reason_size, "config-dir-failed");
                *cursor = '/';
                return -1;
            }
        } else if (S_ISLNK(status.st_mode)) {
            copy_string(reason, reason_size, "config-dir-symlink");
            *cursor = '/';
            return -1;
        }
        *cursor = '/';
    }
    if (lstat(parent, &status) < 0) {
        if (errno != ENOENT || mkdir(parent, 0700) < 0) {
            copy_string(reason, reason_size, "config-dir-failed");
            return -1;
        }
        if (lstat(parent, &status) < 0) {
            copy_string(reason, reason_size, "config-dir-failed");
            return -1;
        }
    }
    if (S_ISLNK(status.st_mode)) {
        copy_string(reason, reason_size, "config-dir-symlink");
        return -1;
    }
    if (!S_ISDIR(status.st_mode)) {
        copy_string(reason, reason_size, "config-dir-not-dir");
        return -1;
    }
    if (config->require_root_owner && status.st_uid != 0) {
        copy_string(reason, reason_size, "config-dir-owner");
        return -1;
    }
    if (status.st_mode & 077) {
        copy_string(reason, reason_size, "config-dir-permissions");
        return -1;
    }
    return 0;
}

/* fsync a file's parent directory so a rename is durable across power loss. */
static void fsync_parent_dir(const char *path)
{
    char parent[PATH_MAX];
    char *slash;
    int fd;

    if (!path || strlen(path) >= sizeof(parent))
        return;
    copy_string(parent, sizeof(parent), path);
    slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        return;
    *slash = '\0';
    fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
}

int le_recovery_psk_ensure(const struct le_recovery_config *config,
                           char *out, size_t out_size,
                           char *reason, size_t reason_size)
{
    char buffer[LE_RECOVERY_PSK_MAX];
    char temporary[PATH_MAX];
    struct stat status;
    int fd, length;

    if (reason && reason_size)
        reason[0] = '\0';
    if (!config || !config->psk_path[0] || !out || out_size < 2) {
        copy_string(reason, reason_size, "psk-path-missing");
        return -1;
    }
    fd = open(config->psk_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode)) {
            close(fd);
            copy_string(reason, reason_size, "psk-not-regular");
            return -1;
        }
        if (status.st_mode & 077) {
            close(fd);
            copy_string(reason, reason_size, "psk-permissions");
            return -1;
        }
        length = (int)read(fd, buffer, sizeof(buffer) - 1);
        close(fd);
        if (length <= 0) {
            copy_string(reason, reason_size, "psk-unreadable");
            return -1;
        }
        buffer[length] = '\0';
        copy_string(out, out_size, buffer);
        return 0;
    }
    if (errno != ENOENT) {
        copy_string(reason, reason_size,
                    errno == ELOOP ? "psk-symlink" : "psk-open-failed");
        return -1;
    }
    if (generate_psk(buffer, sizeof(buffer)) < 0) {
        copy_string(reason, reason_size, "psk-generation-failed");
        return -1;
    }
    if (ensure_secure_parent_dir(config, config->psk_path, reason,
                                 reason_size) < 0)
        return -1;
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld",
                 config->psk_path, (long)getpid()) >= (int)sizeof(temporary)) {
        copy_string(reason, reason_size, "psk-path-too-long");
        return -1;
    }
    (void)unlink(temporary);
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
              0600);
    if (fd < 0) {
        copy_string(reason, reason_size, "psk-write-failed");
        return -1;
    }
    length = (int)write(fd, buffer, strlen(buffer));
    if (length != (int)strlen(buffer) || fsync(fd) < 0 || close(fd) < 0) {
        close(fd);
        (void)unlink(temporary);
        copy_string(reason, reason_size, "psk-write-failed");
        return -1;
    }
    if (rename(temporary, config->psk_path) < 0) {
        int saved = errno;
        (void)unlink(temporary);
        copy_string(reason, reason_size, "psk-rename-failed");
        errno = saved;
        return -1;
    }
    fsync_parent_dir(config->psk_path);
    copy_string(out, out_size, buffer);
    return 0;
}

/* ----- Capability probe ------------------------------------------------- */

static int probe_command(const struct le_recovery *recovery, const char *path)
{
    char *argv[2];
    char program[PATH_MAX];

    if (!path || !path[0])
        return -1;
    if (!recovery->backend.available ||
        !recovery->backend.available(recovery->backend.ctx, path))
        return -1;
    copy_string(program, sizeof(program), path);
    argv[0] = program;
    argv[1] = NULL;
    if (!recovery->backend.probe)
        return -1;
    return recovery->backend.probe(recovery->backend.ctx, argv);
}

int le_recovery_probe_capabilities(struct le_recovery *recovery,
                                   char *reason, size_t reason_size)
{
    const struct le_recovery_config *config = &recovery->config;
    int have_dhcp;

    if (reason && reason_size)
        reason[0] = '\0';
    recovery->available = 0;
    if (!recovery->backend.available) {
        copy_string(reason, reason_size, "no-backend");
        return -1;
    }
    if (!recovery->backend.available(recovery->backend.ctx,
                                     config->hostapd_bin)) {
        copy_string(reason, reason_size, "missing-hostapd");
        goto unavailable;
    }
    have_dhcp = (config->dhcp_bin[0] &&
                 recovery->backend.available(recovery->backend.ctx,
                                             config->dhcp_bin)) ||
                (config->dns_bin[0] &&
                 recovery->backend.available(recovery->backend.ctx,
                                             config->dns_bin));
    if (!have_dhcp) {
        copy_string(reason, reason_size, "missing-dhcp-dns");
        goto unavailable;
    }
    /* The AP-capable driver path cannot be assumed from the presence of a
     * binary.  Without an explicit probe the feature stays a hardware gate
     * rather than reporting a success this host cannot verify. */
    if (!config->ap_probe_cmd[0]) {
        copy_string(reason, reason_size, "ap-driver-unverified");
        goto unavailable;
    }
    if (probe_command(recovery, config->ap_probe_cmd) < 0) {
        copy_string(reason, reason_size, "ap-unsupported");
        goto unavailable;
    }
    recovery->available = 1;
    copy_string(reason, reason_size, "available");
    return 1;

unavailable:
    recovery->available = 0;
    if (reason && reason_size)
        copy_string(recovery->unavailable_reason,
                    sizeof(recovery->unavailable_reason), reason);
    else
        copy_string(recovery->unavailable_reason,
                    sizeof(recovery->unavailable_reason), "unavailable");
    return 0;
}

/* ----- Child ownership -------------------------------------------------- */

static int spawn_child(struct le_recovery *recovery, const char *name,
                       const char *program, char *const argv[])
{
    struct le_recovery_process *process;
    char err[LE_RECOVERY_REASON_MAX] = "";
    pid_t pid;

    if (recovery->child_count >= LE_RECOVERY_CHILD_MAX) {
        copy_string(recovery->last_error, sizeof(recovery->last_error),
                    "child-limit");
        return -1;
    }
    pid = recovery->backend.spawn(recovery->backend.ctx, argv, err, sizeof(err));
    if (pid <= 0) {
        copy_string(recovery->last_error, sizeof(recovery->last_error),
                    err[0] ? err : "spawn-failed");
        return -1;
    }
    process = &recovery->children[recovery->child_count++];
    memset(process, 0, sizeof(*process));
    process->pid = pid;
    process->active = 1;
    process->start_token = recovery->backend.identity ?
        recovery->backend.identity(recovery->backend.ctx, pid) : 1;
    copy_string(process->name, sizeof(process->name), name);
    if (process->start_token == 0) {
        /* A child whose start token cannot be read cannot be safely owned. */
        recovery->backend.signal_child(recovery->backend.ctx, pid, SIGKILL);
        recovery->backend.wait_child(recovery->backend.ctx, pid, NULL);
        --recovery->child_count;
        copy_string(recovery->last_error, sizeof(recovery->last_error),
                    "child-identity");
        return -1;
    }
    (void)program;
    return 0;
}

static int spawn_hostapd(struct le_recovery *recovery)
{
    char *argv[3];
    char program[PATH_MAX], conf[PATH_MAX];

    copy_string(program, sizeof(program), recovery->config.hostapd_bin);
    copy_string(conf, sizeof(conf), recovery->config.hostapd_conf);
    argv[0] = program;
    argv[1] = conf[0] ? conf : NULL;
    argv[2] = NULL;
    return spawn_child(recovery, "hostapd", recovery->config.hostapd_bin, argv);
}

static int spawn_services(struct le_recovery *recovery)
{
    const struct le_recovery_config *config = &recovery->config;

    if (config->dns_bin[0] &&
        (!config->dhcp_bin[0] || !strcmp(config->dhcp_bin, config->dns_bin))) {
        char *argv[4];
        char program[PATH_MAX], conf[PATH_MAX], noguard[32];
        copy_string(program, sizeof(program), config->dns_bin);
        copy_string(conf, sizeof(conf), config->dhcp_conf);
        copy_string(noguard, sizeof(noguard), "--no-daemon");
        argv[0] = program;
        argv[1] = noguard;
        argv[2] = conf[0] ? conf : NULL;
        argv[3] = NULL;
        return spawn_child(recovery, "dnsmasq", config->dns_bin, argv);
    }
    if (config->dhcp_bin[0]) {
        char *argv[3];
        char program[PATH_MAX], conf[PATH_MAX];
        copy_string(program, sizeof(program), config->dhcp_bin);
        copy_string(conf, sizeof(conf), config->dhcp_conf);
        argv[0] = program;
        argv[1] = conf[0] ? conf : NULL;
        argv[2] = NULL;
        if (spawn_child(recovery, "dhcp", config->dhcp_bin, argv) < 0)
            return -1;
    }
    if (config->dns_bin[0] && !config->dhcp_bin[0]) {
        char *argv[2];
        char program[PATH_MAX];
        copy_string(program, sizeof(program), config->dns_bin);
        argv[0] = program;
        argv[1] = NULL;
        return spawn_child(recovery, "dns", config->dns_bin, argv);
    }
    return 0;
}

static int children_alive(struct le_recovery *recovery, char *reason,
                          size_t reason_size)
{
    int i;

    for (i = 0; i < recovery->child_count; ++i) {
        struct le_recovery_process *process = &recovery->children[i];
        int status = 0, result;

        if (!process->active)
            continue;
        result = recovery->backend.wait_child(recovery->backend.ctx,
                                              process->pid, &status);
        if (result == 0) {
            process->active = 0;
            copy_string(reason, reason_size, "child-exited");
            return 0;
        }
        if (result < 0) {
            copy_string(reason, reason_size, "child-wait-error");
            return 0;
        }
    }
    return 1;
}

/*
 * Run the platform interface helper with the documented argument vector.  The
 * helper runs to completion before the caller proceeds, so its exit status
 * gates the state machine: the portal IPv4 must be assigned (net-up) before
 * hostapd/dnsmasq start and removed again (net-down) on teardown.  A missing
 * helper or a non-zero exit fails closed rather than pretending an AP exists.
 */
static int run_net_helper(struct le_recovery *recovery, const char *path, int up)
{
    char program[PATH_MAX], interface[64], address[64];
    char *argv[7];

    if (!path || !path[0] || !recovery->backend.probe)
        return -1;
    if (recovery->backend.available &&
        !recovery->backend.available(recovery->backend.ctx, path))
        return -1;
    copy_string(program, sizeof(program), path);
    copy_string(interface, sizeof(interface), recovery->config.interface);
    argv[0] = program;
    argv[1] = "--interface";
    argv[2] = interface;
    if (up) {
        snprintf(address, sizeof(address), "%s/%s",
                 recovery->config.ap_address, LE_RECOVERY_AP_PREFIX_DEFAULT);
        argv[3] = "--address";
        argv[4] = address;
        argv[5] = NULL;
    } else {
        argv[3] = NULL;
    }
    return recovery->backend.probe(recovery->backend.ctx, argv);
}

static void teardown_children(struct le_recovery *recovery, long long now_ms)
{
    const long long stop_ms = recovery->config.stop_timeout_ms;
    long long deadline = now_ms + stop_ms;
    int i;

    for (i = 0; i < recovery->child_count; ++i)
        if (recovery->children[i].active)
            (void)recovery->backend.signal_child(
                recovery->backend.ctx, recovery->children[i].pid, SIGTERM);
    for (;;) {
        int all_gone = 1;
        for (i = 0; i < recovery->child_count; ++i) {
            struct le_recovery_process *process = &recovery->children[i];
            if (!process->active)
                continue;
            if (recovery->backend.wait_child(recovery->backend.ctx,
                                             process->pid, NULL) != 0)
                all_gone = 0;
            else
                process->active = 0;
        }
        if (all_gone || now_ms >= deadline)
            break;
        {
            struct timespec pause = { 0, 20 * 1000 * 1000 };
            nanosleep(&pause, NULL);
        }
        now_ms += 20;
    }
    for (i = 0; i < recovery->child_count; ++i) {
        struct le_recovery_process *process = &recovery->children[i];
        if (!process->active)
            continue;
        (void)recovery->backend.signal_child(recovery->backend.ctx,
                                             process->pid, SIGKILL);
        (void)recovery->backend.wait_child(recovery->backend.ctx,
                                           process->pid, NULL);
        process->active = 0;
    }
    recovery->child_count = 0;
    /* The portal IPv4 and radio ownership are only ours between net-up and
     * net-down; restore them exactly once whenever net-up succeeded. */
    if (recovery->net_configured) {
        (void)run_net_helper(recovery, recovery->config.net_down_cmd, 0);
        recovery->net_configured = 0;
    }
}

/* ----- Lifecycle -------------------------------------------------------- */

static void build_ssid(struct le_recovery *recovery);

/* Atomically write a root-only configuration/secret file.  The temporary file
 * is created in the destination directory with O_NOFOLLOW so a pre-planted
 * symlink can never redirect the write, fsynced, then renamed over the target.
 * Returns 0 on success; the caller owns the reason text. */
static int write_locked_file(const char *path, const char *data,
                             char *reason, size_t reason_size)
{
    char temporary[PATH_MAX];
    int fd, n, length = (int)strlen(data);

    if (!path || !path[0]) {
        copy_string(reason, reason_size, "missing-config-path");
        return -1;
    }
    if (ensure_parent_dir(path) < 0) {
        copy_string(reason, reason_size, "config-dir-unavailable");
        return -1;
    }
    if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
                 (long)getpid()) >= (int)sizeof(temporary)) {
        copy_string(reason, reason_size, "config-path-too-long");
        return -1;
    }
    (void)unlink(temporary);
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
              0600);
    if (fd < 0) {
        copy_string(reason, reason_size, "config-open-failed");
        return -1;
    }
    n = (int)write(fd, data, length);
    if (n != length) {
        close(fd);
        (void)unlink(temporary);
        copy_string(reason, reason_size, "config-write-failed");
        return -1;
    }
    (void)fsync(fd);
    close(fd);
    if (rename(temporary, path) < 0) {
        (void)unlink(temporary);
        copy_string(reason, reason_size, "config-rename-failed");
        return -1;
    }
    return 0;
}

/*
 * Generate the per-boot softAP configuration.  The SSID and the WPA2
 * passphrase are per-device and must never be constant, so they cannot ship as
 * a static file: they are written here, on the tmpfs run directory, mode 0600,
 * immediately before hostapd starts.  The DHCP/DNS side only hands out a
 * bounded lease and resolves every name to the portal address; it does not
 * create an account, reset anything or bypass the web layer's owner
 * authentication and CSRF checks.
 */
static int write_recovery_configs(struct le_recovery *recovery,
                                  char *reason, size_t reason_size)
{
    char psk[LE_RECOVERY_PSK_MAX];
    char content[1536];
    char base[32], range[128];
    char *dot;
    int written;

    if (le_recovery_psk_ensure(&recovery->config, psk, sizeof(psk),
                               reason, reason_size) < 0)
        return -1;
    if (!recovery->ssid[0])
        build_ssid(recovery);
    copy_string(base, sizeof(base), recovery->config.ap_address);
    dot = strrchr(base, '.');
    if (dot)
        *dot = '\0';
    snprintf(range, sizeof(range), "%s.10,%s.200,255.255.255.0,12h",
             base, base);
    written = snprintf(
        content, sizeof(content),
        "interface=%s\n"
        "driver=nl80211\n"
        "ctrl_interface=%s\n"
        "ssid=%s\n"
        "hw_mode=g\n"
        "channel=6\n"
        "ieee80211n=1\n"
        "wpa=2\n"
        "wpa_passphrase=%s\n"
        "wpa_key_mgmt=WPA-PSK\n"
        "wpa_pairwise=CCMP\n"
        "rsn_pairwise=CCMP\n"
        "auth_algs=1\n"
        "ignore_broadcast_ssid=0\n"
        "beacon_int=100\n",
        recovery->config.interface, recovery->config.run_dir,
        recovery->ssid, psk);
    memset(psk, 0, sizeof(psk));
    if (written < 0 || written >= (int)sizeof(content)) {
        copy_string(reason, reason_size, "hostapd-config-overflow");
        return -1;
    }
    if (write_locked_file(recovery->config.hostapd_conf, content,
                          reason, reason_size) < 0)
        return -1;
    written = snprintf(
        content, sizeof(content),
        "interface=%s\n"
        "bind-interfaces\n"
        "no-resolv\n"
        "no-hosts\n"
        "domain-needed\n"
        "bogus-priv\n"
        "dhcp-range=%s\n"
        "dhcp-option=3,%s\n"
        "dhcp-option=6,%s\n"
        "address=/#/%s\n"
        "log-dhcp\n",
        recovery->config.interface, range, recovery->config.ap_address,
        recovery->config.ap_address, recovery->config.ap_address);
    if (written < 0 || written >= (int)sizeof(content)) {
        copy_string(reason, reason_size, "dnsmasq-config-overflow");
        return -1;
    }
    return write_locked_file(recovery->config.dhcp_conf, content,
                             reason, reason_size);
}

static int start_services(struct le_recovery *recovery, long long now_ms)
{
    char reason[LE_RECOVERY_REASON_MAX] = "";

    if (le_recovery_probe_capabilities(recovery, reason, sizeof(reason)) <= 0) {
        copy_string(recovery->unavailable_reason,
                    sizeof(recovery->unavailable_reason),
                    reason[0] ? reason : "unavailable");
        recovery->mode = LE_RECOVERY_MODE_UNAVAILABLE;
        return -1;
    }
    /* The per-device SSID/passphrase config must exist before hostapd is
     * launched; a failure here is a capability failure and fails closed. */
    if (write_recovery_configs(recovery, reason, sizeof(reason)) < 0) {
        copy_string(recovery->unavailable_reason,
                    sizeof(recovery->unavailable_reason),
                    reason[0] ? reason : "config-unavailable");
        recovery->mode = LE_RECOVERY_MODE_UNAVAILABLE;
        return -1;
    }
    teardown_children(recovery, now_ms);
    /* The portal IPv4 must be assigned and the single radio released from
     * client STA management before dnsmasq can bind and lease.  Without this
     * step the AP would appear configured but never actually serve a lease,
     * so a missing or failing helper fails closed. */
    if (!recovery->config.net_up_cmd[0]) {
        copy_string(recovery->unavailable_reason,
                    sizeof(recovery->unavailable_reason), "ap-net-unconfigured");
        recovery->mode = LE_RECOVERY_MODE_UNAVAILABLE;
        return -1;
    }
    if (run_net_helper(recovery, recovery->config.net_up_cmd, 1) < 0) {
        copy_string(recovery->unavailable_reason,
                    sizeof(recovery->unavailable_reason), "ap-net-up-failed");
        recovery->mode = LE_RECOVERY_MODE_UNAVAILABLE;
        return -1;
    }
    recovery->net_configured = 1;
    if (spawn_hostapd(recovery) < 0 || spawn_services(recovery) < 0) {
        teardown_children(recovery, now_ms);
        recovery->mode = LE_RECOVERY_MODE_UNAVAILABLE;
        return -1;
    }
    recovery->mode = LE_RECOVERY_MODE_STARTING;
    recovery->start_deadline_ms = now_ms + recovery->config.start_timeout_ms;
    return 0;
}

static int verify_ready(struct le_recovery *recovery)
{
    char reason[LE_RECOVERY_REASON_MAX] = "";

    if (!children_alive(recovery, reason, sizeof(reason))) {
        copy_string(recovery->last_error, sizeof(recovery->last_error),
                    reason);
        return -1;
    }
    if (recovery->config.ready_probe_cmd[0] &&
        probe_command(recovery, recovery->config.ready_probe_cmd) < 0) {
        copy_string(recovery->last_error, sizeof(recovery->last_error),
                    "not-serving");
        return -1;
    }
    return 0;
}

int le_recovery_arm(struct le_recovery *recovery,
                    enum le_recovery_trigger trigger, long long now_ms)
{
    char psk[LE_RECOVERY_PSK_MAX];
    char reason[LE_RECOVERY_REASON_MAX] = "";

    if (!recovery || recovery->mode == LE_RECOVERY_MODE_ACTIVE ||
        recovery->mode == LE_RECOVERY_MODE_STARTING)
        return -1;
    if (!recovery->config.enabled) {
        copy_string(recovery->last_error, sizeof(recovery->last_error),
                    "recovery-disabled");
        return -1;
    }
    recovery->trigger = trigger;
    recovery->mode = LE_RECOVERY_MODE_ARMED;
    recovery->armed_at_ms = now_ms;
    recovery->last_activity_ms = now_ms;
    recovery->last_now_ms = now_ms;
    recovery->last_error[0] = '\0';
    copy_string(recovery->unavailable_reason,
                sizeof(recovery->unavailable_reason), "");
    /* Require the per-device secret to exist before the AP can be announced.
     * The value stays local; a failure fails closed rather than activating
     * with an unusable provisioning password. */
    if (le_recovery_psk_ensure(&recovery->config, psk, sizeof(psk),
                               reason, sizeof(reason)) < 0) {
        memset(psk, 0, sizeof(psk));
        copy_string(recovery->last_error, sizeof(recovery->last_error),
                    reason[0] ? reason : "psk-unavailable");
        recovery->mode = LE_RECOVERY_MODE_UNAVAILABLE;
        return -1;
    }
    memset(psk, 0, sizeof(psk));
    return 0;
}

static void format_ssid(const struct le_recovery *recovery, char *out,
                        size_t out_size)
{
    char serial[64] = "";
    char suffix[5];
    int fd, i;

    fd = open(recovery->config.serial_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        int n = (int)read(fd, serial, sizeof(serial) - 1);
        close(fd);
        if (n > 0)
            serial[n] = '\0';
    }
    for (i = 0; serial[i]; ++i) {
        char c = serial[i];
        if (c == '\n' || c == '\r') {
            serial[i] = '\0';
            break;
        }
    }
    /* Non-identifying suffix: the SSID is public, but must not leak a longer
     * serial.  Four trailing characters are the documented contract. */
    if (strlen(serial) >= 4)
        snprintf(suffix, sizeof(suffix), "%s", serial + strlen(serial) - 4);
    else
        snprintf(suffix, sizeof(suffix), "0000");
    snprintf(out, out_size, "LibreEcho-Setup-%s", suffix);
}

static void build_ssid(struct le_recovery *recovery)
{
    format_ssid(recovery, recovery->ssid, sizeof(recovery->ssid));
}

int le_recovery_tick(struct le_recovery *recovery, long long now_ms,
                     int associated)
{
    int transitioned = 0;

    if (!recovery)
        return 0;
    recovery->last_now_ms = now_ms;
    if (associated && (recovery->mode == LE_RECOVERY_MODE_ACTIVE ||
                       recovery->mode == LE_RECOVERY_MODE_STARTING)) {
        le_recovery_stop(recovery, now_ms, "associated");
        return 1;
    }
    switch (recovery->mode) {
    case LE_RECOVERY_MODE_ARMED:
        if (!recovery->ssid[0])
            build_ssid(recovery);
        (void)start_services(recovery, now_ms);
        transitioned = 1;
        break;
    case LE_RECOVERY_MODE_STARTING:
        if (now_ms >= recovery->start_deadline_ms) {
            if (verify_ready(recovery) == 0) {
                recovery->mode = LE_RECOVERY_MODE_ACTIVE;
                recovery->active_since_ms = now_ms;
                recovery->secret_available = 1;
                recovery->led_active = 1;
            } else {
                recovery->mode = LE_RECOVERY_MODE_UNAVAILABLE;
                recovery->led_active = 0;
                teardown_children(recovery, now_ms);
            }
            transitioned = 1;
        } else if (!children_alive(recovery, recovery->last_error,
                                   sizeof(recovery->last_error))) {
            recovery->mode = LE_RECOVERY_MODE_UNAVAILABLE;
            teardown_children(recovery, now_ms);
            transitioned = 1;
        }
        break;
    case LE_RECOVERY_MODE_CLIENT:
    case LE_RECOVERY_MODE_UNAVAILABLE:
    case LE_RECOVERY_MODE_STOPPED:
        if (recovery->config.enabled && recovery->config.auto_enabled &&
            !associated && !recovery->auto_counting) {
            recovery->auto_counting = 1;
            recovery->auto_started_ms = now_ms;
            recovery->auto_deadline_ms =
                now_ms + recovery->config.auto_timeout_ms;
        }
        if (recovery->auto_counting && associated) {
            recovery->auto_counting = 0;
        } else if (recovery->auto_counting && now_ms >= recovery->auto_deadline_ms) {
            recovery->auto_counting = 0;
            le_recovery_arm(recovery, LE_RECOVERY_TRIGGER_AUTO, now_ms);
            transitioned = 1;
        }
        break;
    default:
        break;
    }
    return transitioned;
}

void le_recovery_stop(struct le_recovery *recovery, long long now_ms,
                      const char *reason)
{
    if (!recovery)
        return;
    recovery->mode = LE_RECOVERY_MODE_STOPPING;
    teardown_children(recovery, now_ms);
    if (recovery->trigger == LE_RECOVERY_TRIGGER_PHYSICAL &&
        recovery->config.marker_path[0])
        (void)unlink(recovery->config.marker_path);
    recovery->led_active = 0;
    recovery->secret_available = 0;
    recovery->auto_counting = 0;
    recovery->mode = LE_RECOVERY_MODE_STOPPED;
    recovery->trigger = LE_RECOVERY_TRIGGER_NONE;
    recovery->last_activity_ms = now_ms;
    if (reason)
        copy_string(recovery->last_error, sizeof(recovery->last_error),
                    reason);
}

int le_recovery_config_validate(const struct le_recovery_config *config,
                                char *reason, size_t reason_size)
{
    if (reason && reason_size)
        reason[0] = '\0';
    if (!config) {
        copy_string(reason, reason_size, "config-missing");
        return -1;
    }
    if (config->enabled != 0 && config->enabled != 1) {
        copy_string(reason, reason_size, "enabled-invalid");
        return -1;
    }
    if (config->auto_enabled != 0 && config->auto_enabled != 1) {
        copy_string(reason, reason_size, "auto-enabled-invalid");
        return -1;
    }
    if (config->auto_timeout_ms < LE_RECOVERY_TIMEOUT_MIN_MS ||
        config->auto_timeout_ms > LE_RECOVERY_TIMEOUT_MAX_MS) {
        /* Strict: an out-of-range owner value is rejected, never clamped. */
        copy_string(reason, reason_size, "auto-timeout-range");
        return -1;
    }
    return 0;
}

int le_recovery_config_persist(const struct le_recovery_config *config,
                               char *reason, size_t reason_size)
{
    char content[256];
    int n;

    if (reason && reason_size)
        reason[0] = '\0';
    if (!config || !config->config_path[0]) {
        copy_string(reason, reason_size, "config-path-missing");
        return -1;
    }
    if (le_recovery_config_validate(config, reason, reason_size) < 0)
        return -1;
    n = snprintf(content, sizeof(content),
                 "enabled=%d\nauto_enabled=%d\nauto_timeout_ms=%lld\n",
                 config->enabled, config->auto_enabled,
                 config->auto_timeout_ms);
    if (n < 0 || n >= (int)sizeof(content)) {
        copy_string(reason, reason_size, "config-overflow");
        return -1;
    }
    if (ensure_secure_parent_dir(config, config->config_path, reason,
                                 reason_size) < 0)
        return -1;
    if (write_locked_file(config->config_path, content, reason,
                          reason_size) < 0)
        return -1;
    fsync_parent_dir(config->config_path);
    return 0;
}

int le_recovery_config_load(struct le_recovery_config *config,
                            char *reason, size_t reason_size)
{
    char buffer[512];
    char *line, *save = NULL;
    long long timeout = config ? config->auto_timeout_ms : 0;
    int enabled = config ? config->enabled : 1;
    int auto_enabled = config ? config->auto_enabled : 0;
    int seen_enabled = 0, seen_auto = 0, seen_timeout = 0;
    struct stat status;
    ssize_t length;
    int fd;

    if (reason && reason_size)
        reason[0] = '\0';
    if (!config || !config->config_path[0]) {
        copy_string(reason, reason_size, "config-path-missing");
        return -1;
    }
    fd = open(config->config_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT)
            return 0;   /* no persisted configuration yet */
        copy_string(reason, reason_size,
                    errno == ELOOP ? "config-symlink" : "config-open-failed");
        return -1;
    }
    if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode)) {
        close(fd);
        copy_string(reason, reason_size, "config-not-regular");
        return -1;
    }
    if (status.st_mode & 077) {
        close(fd);
        copy_string(reason, reason_size, "config-permissions");
        return -1;
    }
    length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0) {
        copy_string(reason, reason_size, "config-empty");
        return -1;
    }
    buffer[length] = '\0';
    for (line = strtok_r(buffer, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *value = strchr(line, '=');
        if (!value) {
            copy_string(reason, reason_size, "config-malformed");
            return -1;
        }
        *value++ = '\0';
        if (!strcmp(line, "enabled")) {
            if (strcmp(value, "0") && strcmp(value, "1"))
                goto malformed;
            enabled = value[0] - '0';
            seen_enabled = 1;
        } else if (!strcmp(line, "auto_enabled")) {
            if (strcmp(value, "0") && strcmp(value, "1"))
                goto malformed;
            auto_enabled = value[0] - '0';
            seen_auto = 1;
        } else if (!strcmp(line, "auto_timeout_ms")) {
            char *end = NULL;
            timeout = strtoll(value, &end, 10);
            if (!end || *end || timeout < LE_RECOVERY_TIMEOUT_MIN_MS ||
                timeout > LE_RECOVERY_TIMEOUT_MAX_MS)
                goto malformed;
            seen_timeout = 1;
        } else {
            copy_string(reason, reason_size, "config-unknown-key");
            return -1;
        }
    }
    if (!seen_enabled && !seen_auto && !seen_timeout) {
        copy_string(reason, reason_size, "config-empty");
        return -1;
    }
    config->enabled = enabled;
    config->auto_enabled = auto_enabled;
    config->auto_timeout_ms = timeout;
    copy_string(reason, reason_size, "config-loaded");
    return 1;

malformed:
    copy_string(reason, reason_size, "config-malformed");
    return -1;
}

int le_recovery_configure(struct le_recovery *recovery, int enabled,
                          int auto_enabled, long long auto_timeout_ms,
                          char *reason, size_t reason_size)
{
    struct le_recovery_config candidate;

    if (reason && reason_size)
        reason[0] = '\0';
    if (!recovery) {
        copy_string(reason, reason_size, "config-missing");
        return -1;
    }
    candidate = recovery->config;
    candidate.enabled = enabled;
    candidate.auto_enabled = auto_enabled;
    candidate.auto_timeout_ms = auto_timeout_ms;
    if (le_recovery_config_validate(&candidate, reason, reason_size) < 0)
        return -1;
    /* Persist first; a failed write must not change the running configuration
     * (rollback) so an owner request that cannot be stored never takes partial
     * effect for this boot. */
    if (le_recovery_config_persist(&candidate, reason, reason_size) < 0)
        return -1;
    recovery->config.enabled = candidate.enabled;
    recovery->config.auto_enabled = candidate.auto_enabled;
    recovery->config.auto_timeout_ms = candidate.auto_timeout_ms;
    if (!recovery->config.enabled)
        le_recovery_stop(recovery, recovery->last_now_ms, "disabled");
    return 0;
}

int le_recovery_handover_begin(struct le_recovery *recovery, long long now_ms)
{
    if (!recovery || !recovery->config.enabled)
        return 0;
    if (recovery->mode != LE_RECOVERY_MODE_ACTIVE &&
        recovery->mode != LE_RECOVERY_MODE_STARTING &&
        recovery->mode != LE_RECOVERY_MODE_ARMED)
        return 0;
    /* One radio: free it and remove the portal IPv4 before wpa_supplicant
     * tries to associate.  The boot marker is kept so a failed attempt can
     * rebuild the AP. */
    teardown_children(recovery, now_ms);
    recovery->led_active = 0;
    recovery->secret_available = 0;
    recovery->auto_counting = 0;
    recovery->mode = LE_RECOVERY_MODE_HANDOVER;
    recovery->last_activity_ms = now_ms;
    return 1;
}

void le_recovery_handover_result(struct le_recovery *recovery, long long now_ms,
                                 int associated)
{
    if (!recovery || recovery->mode != LE_RECOVERY_MODE_HANDOVER)
        return;
    if (associated) {
        le_recovery_stop(recovery, now_ms, "associated");
        return;
    }
    /* Failed attempt: rebuild the AP for another try; marker kept. */
    recovery->mode = LE_RECOVERY_MODE_ARMED;
    recovery->armed_at_ms = now_ms;
    recovery->last_activity_ms = now_ms;
    recovery->auto_counting = 0;
    copy_string(recovery->last_error, sizeof(recovery->last_error),
                "association-failed");
}

int le_recovery_rate_limit(struct le_recovery *recovery, long long now_ms)
{
    if (!recovery)
        return 0;
    if (recovery->rate_window_start_ms == 0 ||
        now_ms - recovery->rate_window_start_ms >
            recovery->config.rate_window_ms) {
        recovery->rate_window_start_ms = now_ms;
        recovery->rate_count = 0;
    }
    if (recovery->rate_count >= recovery->config.rate_max)
        return -1;
    recovery->rate_count++;
    return 0;
}

/* ----- Serialization ---------------------------------------------------- */

const char *le_recovery_mode_name(enum le_recovery_mode mode)
{
    switch (mode) {
    case LE_RECOVERY_MODE_CLIENT:
        return "client";
    case LE_RECOVERY_MODE_ARMED:
        return "armed";
    case LE_RECOVERY_MODE_STARTING:
        return "starting";
    case LE_RECOVERY_MODE_ACTIVE:
        return "recovery-ap";
    case LE_RECOVERY_MODE_HANDOVER:
        return "handover";
    case LE_RECOVERY_MODE_UNAVAILABLE:
        return "unavailable";
    case LE_RECOVERY_MODE_STOPPING:
        return "stopping";
    case LE_RECOVERY_MODE_STOPPED:
        return "stopped";
    }
    return "client";
}

const char *le_recovery_trigger_name(enum le_recovery_trigger trigger)
{
    switch (trigger) {
    case LE_RECOVERY_TRIGGER_PHYSICAL:
        return "physical";
    case LE_RECOVERY_TRIGGER_AUTO:
        return "auto";
    case LE_RECOVERY_TRIGGER_NONE:
        break;
    }
    return "none";
}

int le_recovery_status_json(const struct le_recovery *recovery,
                            char *out, size_t out_size)
{
    size_t n = 0;
    long long countdown = 0;

    if (!recovery || !out || out_size < 2)
        return -1;
    if (recovery->auto_counting)
        countdown = recovery->auto_deadline_ms - recovery->last_now_ms;
    if (countdown < 0)
        countdown = 0;
    if (append_text(out, out_size, &n, "{\"mode\":") < 0 ||
        append_json_string(out, out_size, &n,
                           le_recovery_mode_name(recovery->mode)) < 0 ||
        append_text(out, out_size, &n, ",\"trigger\":") < 0 ||
        append_json_string(out, out_size, &n,
                           le_recovery_trigger_name(recovery->trigger)) < 0 ||
        append_text(out, out_size, &n,
                    ",\"available\":%s,\"ssid\":",
                    recovery->available ? "true" : "false") < 0 ||
        append_json_string(out, out_size, &n, recovery->ssid) < 0 ||
        append_text(out, out_size, &n,
                    ",\"reason\":") < 0 ||
        append_json_string(out, out_size, &n,
                           recovery->unavailable_reason[0] ?
                           recovery->unavailable_reason : recovery->last_error) < 0 ||
        append_text(out, out_size, &n,
                    ",\"error\":") < 0 ||
        append_json_string(out, out_size, &n, recovery->last_error) < 0 ||
        append_text(out, out_size, &n,
                    ",\"secret_available\":%s,\"led_owner\":",
                    recovery->secret_available ? "true" : "false") < 0 ||
        append_json_string(out, out_size, &n,
                           recovery->led_active ? LE_RECOVERY_LED_OWNER : "") < 0 ||
        append_text(out, out_size, &n,
                    ",\"enabled\":%s,\"net_configured\":%s,"
                    "\"auto_enabled\":%s,\"auto_timeout_ms\":%lld,"
                    "\"auto_pending\":%s,\"auto_countdown_ms\":%lld,"
                    "\"rate_count\":%d,\"children\":%d}",
                    recovery->config.enabled ? "true" : "false",
                    recovery->net_configured ? "true" : "false",
                    recovery->config.auto_enabled ? "true" : "false",
                    recovery->config.auto_timeout_ms,
                    recovery->auto_counting ? "true" : "false",
                    countdown,
                    recovery->rate_count, recovery->child_count) < 0)
        return -1;
    return (int)n;
}

/*
 * The provisioning secret may be prepared/revealed only for an owner request
 * made while the device is NOT serving the captive AP: during client mode the
 * owner can save the password in advance (there is no way to retrieve it from
 * the protected AP afterwards), while an unauthenticated captive client must
 * never be able to obtain it.
 */
static int secret_reveal_allowed(const struct le_recovery *recovery)
{
    return recovery->mode != LE_RECOVERY_MODE_ACTIVE &&
           recovery->mode != LE_RECOVERY_MODE_STARTING;
}

int le_recovery_secret_prepare(struct le_recovery *recovery,
                               char *reason, size_t reason_size)
{
    char psk[LE_RECOVERY_PSK_MAX];

    if (reason && reason_size)
        reason[0] = '\0';
    if (!recovery || !recovery->config.enabled) {
        copy_string(reason, reason_size, "recovery-disabled");
        return -1;
    }
    if (!secret_reveal_allowed(recovery)) {
        copy_string(reason, reason_size, "captive-ap-active");
        return -1;
    }
    if (le_recovery_psk_ensure(&recovery->config, psk, sizeof(psk),
                               reason, reason_size) < 0)
        return -1;
    memset(psk, 0, sizeof(psk));
    return 0;
}

int le_recovery_secret_json(const struct le_recovery *recovery,
                            char *out, size_t out_size)
{
    char psk[LE_RECOVERY_PSK_MAX];
    char ssid[LE_RECOVERY_SSID_MAX];
    char reason[LE_RECOVERY_REASON_MAX] = "";
    size_t n = 0;

    if (!recovery || !out || out_size < 2)
        return -1;
    if (!recovery->config.enabled || !secret_reveal_allowed(recovery)) {
        copy_string(out, out_size, "{}");
        return -1;
    }
    if (le_recovery_psk_ensure(&recovery->config, psk, sizeof(psk),
                               reason, sizeof(reason)) < 0) {
        copy_string(out, out_size, "{}");
        return -1;
    }
    /* The owner saving the password needs to know which SSID it unlocks. */
    format_ssid(recovery, ssid, sizeof(ssid));
    if (append_text(out, out_size, &n, "{\"ssid\":") < 0 ||
        append_json_string(out, out_size, &n, ssid) < 0 ||
        append_text(out, out_size, &n, ",\"psk\":") < 0 ||
        append_json_string(out, out_size, &n, psk) < 0 ||
        append_text(out, out_size, &n, "}") < 0) {
        memset(psk, 0, sizeof(psk));
        return -1;
    }
    memset(psk, 0, sizeof(psk));
    return (int)n;
}
