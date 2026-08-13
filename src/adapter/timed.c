#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_PEERS 4
#define MAX_PEER_LEN 253
#define VALID_CLOCK_EPOCH 1577836800
#define NTPD_TERM_GRACE_MS 1000

struct options {
    const char *ntpd;
    const char *hwclock;
    const char *config;
    const char *persistent_config;
    const char *status;
    const char *rtc_device;
    const char *rtc_sysfs_dev;
    const char *route_file;
    const char *resolv_file;
    const char *network_ready_file;
    int retry_seconds;
    int ntpd_timeout_seconds;
    int one_shot;
    int sync_hook;
};

static volatile sig_atomic_t stopping;
static volatile sig_atomic_t child_pid;
static volatile sig_atomic_t child_timed_out;

static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return 0;
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static void on_alarm(int signo)
{
    (void)signo;
    child_timed_out = 1;
    if (child_pid > 1)
        kill((pid_t)child_pid, SIGTERM);
}

static void on_signal(int signo)
{
    (void)signo;
    stopping = 1;
    if (child_pid > 1)
        kill((pid_t)child_pid, SIGTERM);
}

static int clock_valid(void)
{
    return time(NULL) >= VALID_CLOCK_EPOCH;
}

static int valid_peer(const char *peer)
{
    size_t i, n;
    if (!peer || !(n = strlen(peer)) || n > MAX_PEER_LEN)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)peer[i];
        if (!(isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':'))
            return 0;
    }
    return 1;
}

static int load_peers_file(const char *path,
                           char peers[MAX_PEERS][MAX_PEER_LEN + 1])
{
    FILE *file;
    char line[512], *start, *end;
    int count = 0;

    file = fopen(path, "r");
    if (!file)
        return -1;
    while (fgets(line, sizeof(line), file)) {
        start = line;
        while (isspace((unsigned char)*start))
            start++;
        if (!*start || *start == '#')
            continue;
        if (!strncmp(start, "server", 6) &&
            isspace((unsigned char)start[6])) {
            start += 6;
            while (isspace((unsigned char)*start))
                start++;
        }
        end = start + strlen(start);
        while (end > start && isspace((unsigned char)end[-1]))
            *--end = '\0';
        if (!valid_peer(start) || count >= MAX_PEERS) {
            fclose(file);
            return -1;
        }
        strcpy(peers[count++], start);
    }
    fclose(file);
    return count ? count : -1;
}

static int load_peers(const struct options *options,
                      char peers[MAX_PEERS][MAX_PEER_LEN + 1],
                      const char **source)
{
    int count;
    if (options->persistent_config &&
        access(options->persistent_config, R_OK) == 0) {
        count = load_peers_file(options->persistent_config, peers);
        if (count > 0) {
            *source = "persistent";
            return count;
        }
        le_log_warn("persistent NTP configuration is invalid; using image defaults");
    }
    count = load_peers_file(options->config, peers);
    if (count > 0) {
        *source = "image";
        return count;
    }
    return -1;
}

static int ensure_parent(const char *path)
{
    char directory[512], *slash;
    size_t n = strlen(path);
    if (!n || n >= sizeof(directory))
        return -1;
    strcpy(directory, path);
    slash = strrchr(directory, '/');
    if (!slash || slash == directory)
        return 0;
    *slash = '\0';
    if (mkdir(directory, 0755) && errno != EEXIST)
        return -1;
    return 0;
}

static int ensure_rtc_node(const char *device, const char *sysfs_dev)
{
    FILE *file;
    unsigned int major_number, minor_number;
    struct stat st;

    if (!stat(device, &st))
        return 1;
    file = fopen(sysfs_dev, "r");
    if (!file)
        return 0;
    if (fscanf(file, "%u:%u", &major_number, &minor_number) != 2) {
        fclose(file);
        return 0;
    }
    fclose(file);
    if (ensure_parent(device))
        return 0;
    if (mknod(device, S_IFCHR | 0660,
              makedev(major_number, minor_number)) && errno != EEXIST)
        return 0;
    if (!strcmp(device, "/dev/rtc0") && access("/dev/rtc", F_OK) != 0 &&
        symlink("rtc0", "/dev/rtc") && errno != EEXIST)
        le_log_pwarn("could not create /dev/rtc compatibility link");
    return stat(device, &st) == 0;
}

static void peers_csv(char *output, size_t size,
                      char peers[MAX_PEERS][MAX_PEER_LEN + 1], int count)
{
    int i;
    size_t used = 0;
    output[0] = '\0';
    for (i = 0; i < count && used + 2 < size; i++) {
        int n = snprintf(output + used, size - used, "%s%s",
                         i ? "," : "", peers[i]);
        if (n < 0 || (size_t)n >= size - used)
            break;
        used += (size_t)n;
    }
}

static int write_status(const char *path, const char *state,
                        const char *source, int synchronized,
                        int rtc_available, int rtc_persisted,
                        long last_sync_epoch, const char *config_source,
                        const char *servers)
{
    char temporary[600], body[1800];
    const char *detail = getenv("LIBREECHO_TIME_ERROR");
    const char *exit_status = getenv("LIBREECHO_TIME_NTP_EXIT");
    int fd, n;
    ssize_t written;

    if (!detail)
        detail = "";
    if (!exit_status)
        exit_status = "";

    if (ensure_parent(path))
        return -1;
    n = snprintf(body, sizeof(body),
                 "state=%s\nsource=%s\nsynchronized=%d\nclock_valid=%d\n"
                 "rtc_available=%d\nrtc_persisted=%d\nlast_sync_epoch=%ld\n"
                 "config_source=%s\nservers=%s\n"
                 "ntpd_exit_status=%s\nlast_error=%s\n",
                 state, source, synchronized, clock_valid(), rtc_available,
                 rtc_persisted, last_sync_epoch, config_source, servers,
                 exit_status, detail);
    if (n < 0 || (size_t)n >= sizeof(body))
        return -1;
    n = snprintf(temporary, sizeof(temporary), "%s.tmp.%ld",
                 path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(temporary))
        return -1;
    fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    written = write(fd, body, strlen(body));
    if (written != (ssize_t)strlen(body) || fsync(fd)) {
        close(fd);
        unlink(temporary);
        return -1;
    }
    if (close(fd)) {
        unlink(temporary);
        return -1;
    }
    if (rename(temporary, path)) {
        unlink(temporary);
        return -1;
    }
    return 0;
}

static int file_contains(const char *path, const char *needle)
{
    FILE *file;
    char line[512];
    file = fopen(path, "r");
    if (!file)
        return 0;
    while (fgets(line, sizeof(line), file)) {
        if (strstr(line, needle)) {
            fclose(file);
            return 1;
        }
    }
    fclose(file);
    return 0;
}

static int network_ready(const struct options *options)
{
    if (options->network_ready_file)
        return access(options->network_ready_file, F_OK) == 0;
    return file_contains(options->route_file, "\t00000000\t") &&
           file_contains(options->resolv_file, "nameserver");
}

static int run_hwclock(const char *program, const char *rtc_device)
{
    pid_t pid;
    int status;
    pid = fork();
    if (pid < 0)
        return 0;
    if (!pid) {
        execl(program, program, "-u", "-w", "-f", rtc_device, (char *)NULL);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int sync_hook(const struct options *options)
{
    const char *servers = getenv("LIBREECHO_TIME_SERVERS");
    const char *config_source = getenv("LIBREECHO_TIME_CONFIG_SOURCE");
    int rtc_available, rtc_persisted;
    long now = (long)time(NULL);

    rtc_available = ensure_rtc_node(options->rtc_device,
                                    options->rtc_sysfs_dev);
    rtc_persisted = rtc_available &&
                    run_hwclock(options->hwclock, options->rtc_device);
    if (rtc_persisted)
        (void)unsetenv("LIBREECHO_TIME_ERROR");
    else
        (void)setenv("LIBREECHO_TIME_ERROR", "RTC persistence failed", 1);
    if (write_status(options->status, "synchronized", "ntp", 1,
                     rtc_available, rtc_persisted, now,
                     config_source ? config_source : "image",
                     servers ? servers : ""))
        return 1;
    le_log_info("NTP synchronized; RTC persistence %s",
                rtc_persisted ? "succeeded" : "failed");
    return rtc_persisted ? 0 : 1;
}

static pid_t start_ntpd(const struct options *options,
                        char peers[MAX_PEERS][MAX_PEER_LEN + 1], int count,
                        int *output_fd)
{
    char *arguments[6 + MAX_PEERS * 2 + 1];
    int pipe_fds[2];
    int i, n = 0;
    pid_t pid;

    if (!output_fd || pipe(pipe_fds) < 0)
        return -1;
    arguments[n++] = (char *)options->ntpd;
    arguments[n++] = "-n";
    arguments[n++] = "-N";
    arguments[n++] = "-q";
    for (i = 0; i < count; i++) {
        arguments[n++] = "-p";
        arguments[n++] = peers[i];
    }
    arguments[n] = NULL;
    pid = fork();
    if (pid < 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        return -1;
    }
    if (!pid) {
        close(pipe_fds[0]);
        if (dup2(pipe_fds[1], STDOUT_FILENO) < 0 ||
            dup2(pipe_fds[1], STDERR_FILENO) < 0)
            _exit(127);
        close(pipe_fds[1]);
        execv(options->ntpd, arguments);
        _exit(127);
    }
    close(pipe_fds[1]);
    if (fcntl(pipe_fds[0], F_SETFL, fcntl(pipe_fds[0], F_GETFL, 0) | O_NONBLOCK) < 0) {
        close(pipe_fds[0]);
        kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);
        return -1;
    }
    *output_fd = pipe_fds[0];
    return pid;
}

static void drain_ntpd_output(int fd, char *output, size_t size)
{
    char buffer[256];
    ssize_t count;
    size_t used;

    if (fd < 0 || !output || size < 2)
        return;
    used = strlen(output);
    while ((count = read(fd, buffer, sizeof(buffer))) > 0) {
        size_t i;
        for (i = 0; i < (size_t)count && used + 1 < size; ++i) {
            unsigned char c = (unsigned char)buffer[i];
            if (c == '\n' || c == '\r' || c == '\t')
                c = ' ';
            if (c >= 32 && c < 127)
                output[used++] = (char)c;
        }
        output[used] = '\0';
    }
}

static int wait_ntpd(pid_t pid, int output_fd, int *status,
                     char *output, size_t output_size, int timeout_seconds)
{
    int result;
    long long grace_deadline = 0;

    if (output && output_size)
        output[0] = '\0';
    child_timed_out = 0;
    alarm((unsigned int)timeout_seconds);
    for (;;) {
        drain_ntpd_output(output_fd, output, output_size);
        result = waitpid(pid, status, WNOHANG);
        if (result == pid)
            break;
        if (result < 0 && errno != EINTR)
            break;
        if (child_timed_out && !grace_deadline) {
            grace_deadline = monotonic_ms() + NTPD_TERM_GRACE_MS;
            le_log_warn("ntpd did not exit after timeout; allowing bounded termination grace");
        }
        if (grace_deadline && monotonic_ms() >= grace_deadline) {
            le_log_warn("ntpd ignored termination; forcing kill");
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, status, 0) < 0 && errno == EINTR)
                ;
            break;
        }
        (void)poll(NULL, 0, 100);
    }
    alarm(0);
    drain_ntpd_output(output_fd, output, output_size);
    close(output_fd);
    return result == pid ? 0 : -1;
}

static void usage(const char *name)
{
    fprintf(stderr,
            "Usage: %s [--foreground] [--one-shot] [--sync-hook]\\n"
            "  [--ntpd PATH] [--hwclock PATH] [--config PATH]\\n"
            "  [--persistent-config PATH] [--status PATH]\\n"
            "  [--rtc-device PATH] [--rtc-sysfs-dev PATH]\\n"
            "  [--network-ready-file PATH] [--retry-seconds N]\\n"
            "  [--ntpd-timeout-seconds N]\\n", name);
}

static int parse_options(int argc, char **argv, struct options *options)
{
    int i;
#define VALUE() do { if (++i >= argc) return -1; } while (0)
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--foreground"))
            continue;
        if (!strcmp(argv[i], "--one-shot"))
            options->one_shot = 1;
        else if (!strcmp(argv[i], "--sync-hook"))
            options->sync_hook = 1;
        else if (!strcmp(argv[i], "--ntpd")) {
            VALUE(); options->ntpd = argv[i];
        } else if (!strcmp(argv[i], "--hwclock")) {
            VALUE(); options->hwclock = argv[i];
        } else if (!strcmp(argv[i], "--config")) {
            VALUE(); options->config = argv[i];
        } else if (!strcmp(argv[i], "--persistent-config")) {
            VALUE(); options->persistent_config = argv[i];
        } else if (!strcmp(argv[i], "--status")) {
            VALUE(); options->status = argv[i];
        } else if (!strcmp(argv[i], "--rtc-device")) {
            VALUE(); options->rtc_device = argv[i];
        } else if (!strcmp(argv[i], "--rtc-sysfs-dev")) {
            VALUE(); options->rtc_sysfs_dev = argv[i];
        } else if (!strcmp(argv[i], "--route-file")) {
            VALUE(); options->route_file = argv[i];
        } else if (!strcmp(argv[i], "--resolv-file")) {
            VALUE(); options->resolv_file = argv[i];
        } else if (!strcmp(argv[i], "--network-ready-file")) {
            VALUE(); options->network_ready_file = argv[i];
        } else if (!strcmp(argv[i], "--retry-seconds")) {
            VALUE(); options->retry_seconds = atoi(argv[i]);
            if (options->retry_seconds < 1 || options->retry_seconds > 3600)
                return -1;
        } else if (!strcmp(argv[i], "--ntpd-timeout-seconds")) {
            VALUE(); options->ntpd_timeout_seconds = atoi(argv[i]);
            if (options->ntpd_timeout_seconds < 1 ||
                options->ntpd_timeout_seconds > 3600)
                return -1;
        } else if (!strcmp(argv[i], "--verbose") ||
                   !strcmp(argv[i], "--debug") ||
                   !strcmp(argv[i], "--quiet")) {
            continue;
        } else {
            return -1;
        }
    }
#undef VALUE
    return 0;
}

int main(int argc, char **argv)
{
    struct options options = {
        "/bin/ntpd", "/bin/hwclock", "/etc/libreecho/ntp.conf",
        "/data/libreecho/config/ntp.conf",
        "/run/libreecho/time.status", "/dev/rtc0",
        "/sys/class/rtc/rtc0/dev", "/proc/net/route", "/etc/resolv.conf",
        NULL, 30, 30, 0, 0
    };
    char peers[MAX_PEERS][MAX_PEER_LEN + 1], servers[1100];
    const char *config_source = "image";
    int count, status, rtc_available;
    int output_fd = -1;
    int hook_result;
    pid_t pid;
    const char *environment;
    char ntpd_error[512];

    le_log_init("timed", argc, argv);
    if ((environment = getenv("LIBREECHO_TIME_STATUS")))
        options.status = environment;
    if ((environment = getenv("LIBREECHO_TIME_RTC_DEVICE")))
        options.rtc_device = environment;
    if ((environment = getenv("LIBREECHO_TIME_RTC_SYSFS_DEV")))
        options.rtc_sysfs_dev = environment;
    if ((environment = getenv("LIBREECHO_TIME_HWCLOCK")))
        options.hwclock = environment;
    if (getenv("LIBREECHO_TIME_HOOK"))
        options.sync_hook = 1;
    if (parse_options(argc, argv, &options)) {
        usage(argv[0]);
        return 2;
    }
    if (options.sync_hook)
        return sync_hook(&options);
    count = load_peers(&options, peers, &config_source);
    if (count < 1) {
        le_log_error("no valid NTP peers configured");
        return 1;
    }
    peers_csv(servers, sizeof(servers), peers, count);
    rtc_available = ensure_rtc_node(options.rtc_device,
                                    options.rtc_sysfs_dev);
    (void)write_status(options.status, "waiting-network",
                       clock_valid() ? "rtc" : "unset", 0,
                       rtc_available, 0, 0, config_source, servers);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGALRM, on_alarm);
    le_log_info("time daemon started (%d peers, %s configuration)",
                count, config_source);
    while (!stopping) {
        while (!stopping && !network_ready(&options))
            sleep(2);
        if (stopping)
            break;
        (void)setenv("LIBREECHO_TIME_STATUS", options.status, 1);
        (void)setenv("LIBREECHO_TIME_RTC_DEVICE", options.rtc_device, 1);
        (void)setenv("LIBREECHO_TIME_RTC_SYSFS_DEV",
                     options.rtc_sysfs_dev, 1);
        (void)setenv("LIBREECHO_TIME_HWCLOCK", options.hwclock, 1);
        (void)setenv("LIBREECHO_TIME_HOOK", "1", 1);
        (void)setenv("LIBREECHO_TIME_SERVERS", servers, 1);
        (void)setenv("LIBREECHO_TIME_CONFIG_SOURCE", config_source, 1);
        (void)write_status(options.status, "synchronizing",
                           clock_valid() ? "rtc" : "unset", 0,
                           rtc_available, 0, 0, config_source, servers);
        pid = start_ntpd(&options, peers, count, &output_fd);
        if (pid < 0) {
            le_log_perr("could not start ntpd");
            (void)setenv("LIBREECHO_TIME_NTP_EXIT", "start-failed", 1);
            (void)setenv("LIBREECHO_TIME_ERROR", "could not start ntpd", 1);
            status = 1;
        } else {
            child_pid = pid;
            if (wait_ntpd(pid, output_fd, &status,
                          ntpd_error, sizeof(ntpd_error),
                          options.ntpd_timeout_seconds) < 0)
                status = 1;
            child_pid = 0;
            if (WIFEXITED(status)) {
                char exit_text[16];
                snprintf(exit_text, sizeof(exit_text), "%d", WEXITSTATUS(status));
                (void)setenv("LIBREECHO_TIME_NTP_EXIT", exit_text, 1);
            } else if (WIFSIGNALED(status)) {
                char signal_text[32];
                snprintf(signal_text, sizeof(signal_text), "signal-%d", WTERMSIG(status));
                (void)setenv("LIBREECHO_TIME_NTP_EXIT", signal_text, 1);
            } else {
                (void)setenv("LIBREECHO_TIME_NTP_EXIT", "unknown", 1);
            }
            if (child_timed_out)
                (void)setenv("LIBREECHO_TIME_ERROR", "ntpd timed out", 1);
            else if (ntpd_error[0])
                (void)setenv("LIBREECHO_TIME_ERROR", ntpd_error, 1);
            else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                (void)setenv("LIBREECHO_TIME_ERROR", "ntpd exited without diagnostics", 1);
        }
        if (stopping)
            break;
        if (pid >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            /*
             * BusyBox ntpd -S is not guaranteed to run when the correction
             * is below its step threshold.  -q gives us an unambiguous
             * successful synchronization result; persist the clock from
             * the parent after that result instead of relying on a callback.
             */
            (void)unsetenv("LIBREECHO_TIME_ERROR");
            (void)setenv("LIBREECHO_TIME_NTP_EXIT", "0", 1);
            hook_result = sync_hook(&options);
            if (hook_result == 0) {
                (void)unsetenv("LIBREECHO_TIME_ERROR");
                if (options.one_shot)
                    return 0;
                sleep(660);
                continue;
            }
            (void)setenv("LIBREECHO_TIME_ERROR", "RTC persistence failed", 1);
            if (options.one_shot)
                return 1;
        } else if (options.one_shot) {
            int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
            (void)write_status(options.status, "failed",
                               clock_valid() ? "rtc" : "unset", 0,
                               rtc_available, 0, 0, config_source, servers);
            return exit_code;
        }
        le_log_warn("ntpd exited; retrying in %d seconds",
                    options.retry_seconds);
        (void)write_status(options.status, "retrying",
                           clock_valid() ? "rtc" : "unset", 0,
                           rtc_available, 0, 0, config_source, servers);
        sleep((unsigned int)options.retry_seconds);
    }
    le_log_info("time daemon stopped");
    return 0;
}
