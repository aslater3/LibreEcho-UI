/*
 * LibreEcho central log daemon.
 *
 * Receives JSON log entries from all services via SOCK_DGRAM Unix socket,
 * writes them to /var/log/libreecho/system.log with rotation.
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "logd.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static char log_dir[256] = LE_LOGD_DIR;
static char log_file[512] = LE_LOGD_FILE;
static char sock_path[256] = LE_LOGD_SOCK;

static void on_signal(int signo)
{
    (void)signo;
    stop_requested = 1;
}

static int make_listener(const char *path)
{
    struct sockaddr_un addr;
    size_t path_len;
    int fd;

    path_len = strlen(path);
    if (path_len >= sizeof(addr.sun_path)) {
        le_log_error("logd: socket path too long: %s", path);
        return -1;
    }

    unlink(path);
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        le_log_perror(LE_LOG_ERROR, "logd: socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, path_len + 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        le_log_perror(LE_LOG_ERROR, "logd: bind %s", path);
        close(fd);
        return -1;
    }

    chmod(path, 0666);
    return fd;
}

static void rotate_logs(void)
{
    char old_path[sizeof(log_file) + 16], new_path[sizeof(log_file) + 16];
    int i;

    /* Remove oldest */
    snprintf(old_path, sizeof(old_path), "%s.%d", log_file, LE_LOGD_MAX_FILES);
    unlink(old_path);

    /* Shift .2 -> .3, .1 -> .2, etc */
    for (i = LE_LOGD_MAX_FILES - 1; i >= 1; i--) {
        snprintf(old_path, sizeof(old_path), "%s.%d", log_file, i);
        snprintf(new_path, sizeof(new_path), "%s.%d", log_file, i + 1);
        rename(old_path, new_path);
    }

    /* Current -> .1 */
    {
        char backup[sizeof(log_file) + 16];
        snprintf(backup, sizeof(backup), "%s.1", log_file);
        rename(log_file, backup);
    }
}

static int open_log_file(void)
{
    int fd = open(log_file, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        le_log_perror(LE_LOG_ERROR, "logd: open %s", log_file);
    }
    return fd;
}

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s [--foreground] [--verbose] [--debug] [--quiet] [--log-dir PATH] [--sock PATH]\n", prog);
}

int main(int argc, char **argv)
{
    int foreground = 0;
    int listen_fd;
    int log_fd;
    struct pollfd pfd;
    char buf[LE_LOGD_MSG_MAX];
    ssize_t n;
    struct stat st;
    int i;

    le_log_init("logd", argc, argv);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        } else if (strcmp(argv[i], "--verbose") == 0 ||
                   strcmp(argv[i], "--debug") == 0 ||
                   strcmp(argv[i], "--quiet") == 0) {
            /* handled by le_log_init */
        } else if (strcmp(argv[i], "--log-dir") == 0 && i + 1 < argc) {
            strncpy(log_dir, argv[++i], sizeof(log_dir) - 1);
            log_dir[sizeof(log_dir) - 1] = '\0';
            snprintf(log_file, sizeof(log_file), "%s/system.log", log_dir);
        } else if (strcmp(argv[i], "--sock") == 0 && i + 1 < argc) {
            strncpy(sock_path, argv[++i], sizeof(sock_path) - 1);
            sock_path[sizeof(sock_path) - 1] = '\0';
        } else {
            usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    /* Create log directory */
    if (mkdir(log_dir, 0755) < 0 && errno != EEXIST) {
        le_log_perror(LE_LOG_ERROR, "logd: mkdir %s", log_dir);
        return EXIT_FAILURE;
    }

    if (!foreground) {
        if (daemon(1, 0) < 0) {
            le_log_perror(LE_LOG_ERROR, "logd: daemon");
            return EXIT_FAILURE;
        }
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    listen_fd = make_listener(sock_path);
    if (listen_fd < 0) {
        return EXIT_FAILURE;
    }

    log_fd = open_log_file();
    if (log_fd < 0) {
        close(listen_fd);
        return EXIT_FAILURE;
    }

    le_log_info("logd: listening on %s, writing to %s", sock_path, log_file);

    pfd.fd = listen_fd;
    pfd.events = POLLIN;

    while (!stop_requested) {
        if (poll(&pfd, 1, 1000) < 0) {
            if (errno == EINTR)
                continue;
            le_log_perror(LE_LOG_ERROR, "logd: poll");
            break;
        }

        if (pfd.revents & POLLIN) {
            n = recv(listen_fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';

                /* Ensure newline */
                if (n > 0 && buf[n - 1] != '\n') {
                    if (n < (ssize_t)sizeof(buf) - 1) {
                        buf[n] = '\n';
                        buf[n + 1] = '\0';
                        n++;
                    }
                }

                /* Write to log file */
                if (write(log_fd, buf, n) < 0) {
                    le_log_perror(LE_LOG_ERROR, "logd: write");
                }

                /* Check rotation */
                if (fstat(log_fd, &st) == 0 && st.st_size > LE_LOGD_MAX_SIZE) {
                    close(log_fd);
                    rotate_logs();
                    log_fd = open_log_file();
                    if (log_fd < 0) {
                        le_log_error("logd: cannot reopen log file after rotation");
                        break;
                    }
                    le_log_info("logd: rotated logs");
                }
            }
        }
    }

    le_log_info("logd: shutting down");
    close(log_fd);
    close(listen_fd);
    unlink(sock_path);
    return EXIT_SUCCESS;
}
