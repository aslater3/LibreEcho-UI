#define _GNU_SOURCE
#include "mdns_lease.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MDNS_CHROOT
#define MDNS_CHROOT "/bin/busybox"
#endif
#ifndef MDNS_OWNER_EXE
#define MDNS_OWNER_EXE "/usr/local/sbin/libreecho-esphomed"
#endif
#define CLIENTS 4
static volatile sig_atomic_t running = 1;
static void stop_signal(int sig) { (void)sig; running = 0; }
static long now_ms(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) < 0) return 0;
    return t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}
static pid_t spawn(const char *root, int bus)
{
    pid_t parent = getpid(), child = fork();
    if (child != 0) return child;
    /* nofork foreground children die with their owner, including SIGKILL. */
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent) _exit(126);
    if (bus)
        execl(MDNS_CHROOT, MDNS_CHROOT, "chroot", root,
              "/usr/bin/dbus-daemon", "--nofork", "--nopidfile",
              "--config-file=/etc/dbus-1/system.conf", (char *)NULL);
    else
        execl(MDNS_CHROOT, MDNS_CHROOT, "chroot", root,
              "/usr/sbin/avahi-daemon", "--no-chroot", "--no-drop-root",
              (char *)NULL);
    _exit(127);
}
static void retire(pid_t *pid)
{
    int i;
    if (*pid <= 0) return;
    (void)kill(*pid, SIGTERM);
    for (i = 0; i < 20; ++i) {
        pid_t result = waitpid(*pid, NULL, WNOHANG);
        if (result == *pid || (result < 0 && errno == ECHILD)) { *pid = 0; return; }
        usleep(25000);
    }
    (void)kill(*pid, SIGKILL);
    while (waitpid(*pid, NULL, 0) < 0 && errno == EINTR) {}
    *pid = 0;
}
static int alive(pid_t *pid)
{
    pid_t result;
    if (*pid <= 0) return 0;
    result = waitpid(*pid, NULL, WNOHANG);
    if (result == 0) return 1;
    if (result == *pid || (result < 0 && errno == ECHILD)) *pid = 0;
    return 0;
}
static int dbus_socket_ready(const char *root)
{
    char path[512];
    struct stat st;

    if (snprintf(path, sizeof(path), "%s/run/dbus/system_bus_socket", root) >=
        (int)sizeof(path))
        return 0;
    return lstat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}
static int runtime_ready(const char *root, pid_t *bus, pid_t *avahi)
{
    return alive(bus) && alive(avahi) && dbus_socket_ready(root);
}
static int status_client(const char *socket_path)
{
    struct sockaddr_un address;
    struct timeval timeout = { 2, 0 };
    char reply[32];
    ssize_t n;
    int fd;

    if (!socket_path || strlen(socket_path) >= sizeof(address.sun_path))
        return 2;
    fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        send(fd, "STATUS/1\n", 9, MSG_NOSIGNAL) != 9) {
        close(fd);
        return 1;
    }
    n = recv(fd, reply, sizeof(reply) - 1, 0);
    close(fd);
    if (n <= 0 || n >= (ssize_t)sizeof(reply))
        return 1;
    reply[n] = 0;
    return strcmp(reply, "running\n") ? 1 : 0;
}
static int peer_owner(int fd)
{
    struct ucred cred;
    socklen_t len = sizeof(cred);
    char path[64], exe[512];
    ssize_t n;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 ||
        cred.uid != geteuid()) return 0;
    (void)snprintf(path, sizeof(path), "/proc/%ld/exe", (long)cred.pid);
    n = readlink(path, exe, sizeof(exe) - 1);
    if (n < 0 || (size_t)n >= sizeof(exe) - 1) return 0;
    exe[n] = 0;
    return !strcmp(exe, MDNS_OWNER_EXE);
}
static int clear_stale(int fd)
{
    DIR *dir = fdopendir(dup(fd));
    struct dirent *entry;
    int result = 0;
    if (!dir) return -1;
    while ((entry = readdir(dir))) {
        if ((!strncmp(entry->d_name, "wyoming-", 8) ||
             !strncmp(entry->d_name, "esphome-", 8) ||
             !strcmp(entry->d_name, "wyoming.service") ||
             !strcmp(entry->d_name, "esphome.service")) &&
            unlinkat(fd, entry->d_name, 0) < 0) result = -1;
    }
    closedir(dir);
    return result;
}
/* Pin the actual connection creator, not a pidfile, argv[0], or inherited fd.
 * Linux 6.1 supports pidfd_open. If unavailable, fail closed. */
static int peer_pidfd(int fd)
{
#ifdef SYS_pidfd_open
    struct ucred cred;
    socklen_t length = sizeof(cred);
    int pidfd;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &length) < 0 ||
        length != sizeof(cred) || cred.pid <= 0 || !peer_owner(fd)) return -1;
    pidfd = (int)syscall(SYS_pidfd_open, cred.pid, 0);
    if (pidfd < 0) return -1;
    if (fcntl(pidfd, F_SETFD, FD_CLOEXEC) < 0 || !peer_owner(fd)) {
        close(pidfd); return -1;
    }
    return pidfd;
#else
    (void)fd; return -1;
#endif
}
static int peer_still_owner(int socket_fd, int pidfd)
{
    struct pollfd descriptor = { pidfd, POLLIN, 0 };
    return pidfd >= 0 && poll(&descriptor, 1, 0) == 0 && peer_owner(socket_fd);
}
int main(int argc, char **argv)
{
    const char *root = "/usr/local/lib/libreecho-mdns/root";
    const char *socket_path = "/run/libreecho/mdns.sock";
    char path[512];
    struct sockaddr_un address;
    struct le_mdns_lease lease;
    struct pollfd fds[CLIENTS + 1];
    int owner_pidfds[CLIENTS];
    long deadlines[CLIENTS] = {0}, retry = 0, bus_started = 0;
    pid_t bus = 0, avahi = 0;
    int lock_fd, dir_fd, server, i, result = 0, status_only = 0;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--root") && i + 1 < argc) root = argv[++i];
        else if (!strcmp(argv[i], "--socket") && i + 1 < argc) socket_path = argv[++i];
        else if (!strcmp(argv[i], "--foreground")) continue;
        else if (!strcmp(argv[i], "--status")) status_only = 1;
        else return 2;
    }
    if (strlen(socket_path) >= sizeof(address.sun_path) || strlen(root) > 400) return 2;
    if (status_only)
        return status_client(socket_path);
    (void)snprintf(path, sizeof(path), "%s/run/mdns.lock", root);
    lock_fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) < 0) return 1;
    (void)snprintf(path, sizeof(path), "%s/etc/avahi/services", root);
    dir_fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir_fd < 0 || clear_stale(dir_fd) < 0) return 1;
    le_mdns_lease_init(&lease, dir_fd);
    server = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server < 0) return 1;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    (void)unlink(socket_path);
    umask(0077);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(server, CLIENTS) < 0) return 1;
    signal(SIGTERM, stop_signal); signal(SIGINT, stop_signal); signal(SIGPIPE, SIG_IGN);
    for (i = 0; i <= CLIENTS; ++i) { fds[i].fd = -1; fds[i].events = POLLIN; }
    for (i = 0; i < CLIENTS; ++i) owner_pidfds[i] = -1;
    fds[0].fd = server;
    while (running) {
        long now = now_ms();
        if (!alive(&bus)) {
            retire(&avahi);
            if (now >= retry) { bus = spawn(root, 1); bus_started = now; retry = now + 2000; }
        } else if (!alive(&avahi) && dbus_socket_ready(root) &&
                   now - bus_started >= 100 && now >= retry) {
            avahi = spawn(root, 0); retry = now + 2000;
        }
        if (poll(fds, CLIENTS + 1, 100) < 0 && errno != EINTR) { result = 1; break; }
        if (fds[0].revents & POLLIN) {
            int fd = accept4(server, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) {
                for (i = 1; i <= CLIENTS && fds[i].fd >= 0; ++i) {}
                if (i > CLIENTS) close(fd);
                else { fds[i].fd = fd; deadlines[i - 1] = now + 2000; }
            }
        }
        for (i = 1; i <= CLIENTS; ++i) {
            char message[LE_MDNS_ESPHOME_PACKET_MAX];
            ssize_t n;
            int close_client = 0;
            if (fds[i].fd < 0) continue;
            if (fds[i].revents & POLLIN) {
                n = recv(fds[i].fd, message, sizeof(message) - 1, MSG_TRUNC);
                if (n <= 0 || n >= (ssize_t)sizeof(message)) {
                    (void)send(fds[i].fd, "error\n", 6, MSG_NOSIGNAL); close_client = 1;
                }
                else {
                    unsigned int port = 0;
                    struct le_mdns_esphome_metadata metadata;
                    message[n] = 0;
                    if (n == 9 && !memcmp(message, "STATUS/1\n", 9)) {
                        const char *status = runtime_ready(root, &bus, &avahi)
                            ? "running\n" : "degraded\n";
                        (void)send(fds[i].fd, status, strlen(status), MSG_NOSIGNAL);
                        close_client = 1;
                    } else {
                        if (owner_pidfds[i - 1] < 0) owner_pidfds[i - 1] = peer_pidfd(fds[i].fd);
                        if (le_mdns_esphome_decode(message, (size_t)n, &port, &metadata) < 0 ||
                            !peer_still_owner(fds[i].fd, owner_pidfds[i - 1]) ||
                            le_mdns_lease_register_esphome(&lease, fds[i].fd, port, &metadata) < 0) {
                            (void)send(fds[i].fd, "error\n", 6, MSG_NOSIGNAL); close_client = 1;
                        } else {
                            if (avahi > 0 && kill(avahi, SIGHUP) < 0) retire(&avahi);
                            (void)send(fds[i].fd, "pending\n", 8, MSG_NOSIGNAL);
                        }
                    }
                }
            }
            if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) close_client = 1;
            if (lease.owner_fd == fds[i].fd &&
                !peer_still_owner(fds[i].fd, owner_pidfds[i - 1])) close_client = 1;
            if (lease.owner_fd != fds[i].fd && now >= deadlines[i - 1]) close_client = 1;
            if (close_client) {
                if (lease.owner_fd == fds[i].fd) {
                    if (le_mdns_lease_withdraw(&lease, fds[i].fd) < 0) {
                        /* A stale record must not remain published after owner loss. */
                        result = 1; running = 0;
                    }
                    if (avahi > 0 && kill(avahi, SIGHUP) < 0) retire(&avahi);
                }
                close(fds[i].fd); fds[i].fd = -1;
                if (owner_pidfds[i - 1] >= 0) close(owner_pidfds[i - 1]);
                owner_pidfds[i - 1] = -1;
            }
        }
    }
    if (lease.owner_fd >= 0) (void)le_mdns_lease_withdraw(&lease, lease.owner_fd);
    retire(&avahi); retire(&bus);
    for (i = 0; i <= CLIENTS; ++i) if (fds[i].fd >= 0) close(fds[i].fd);
    for (i = 0; i < CLIENTS; ++i) if (owner_pidfds[i] >= 0) close(owner_pidfds[i]);
    (void)unlink(socket_path); close(dir_fd); close(lock_fd);
    return result;
}
