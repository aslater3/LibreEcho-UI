#define _POSIX_C_SOURCE 200809L
#define LE_HTTP_SERVER_WORKER_TEST 1

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../src/http_server.c"

static int wait_for_workers(int kind)
{
    struct timespec pause = {0, 1000000L};
    int i;
    for (i = 0; i < 100; i++) {
        le_test_reap_child_workers();
        if (le_test_worker_count(kind) == 0)
            return 0;
        nanosleep(&pause, NULL);
    }
    return -1;
}

static int read_socket_message(int fd)
{
    char message[3] = {0};
    return read(fd, message, sizeof(message) - 1) == 2 &&
           !strcmp(message, "ok");
}

static int test_normal_outer_workers_are_reaped(void)
{
    const int kinds[] = {
        LE_TEST_WORKER_PCM,
        LE_TEST_WORKER_UPDATE_FETCH,
        LE_TEST_WORKER_UPDATE_UPLOAD
    };
    size_t i;

    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        int pair[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
            return -1;
        if (le_test_spawn_noop_worker(pair[0], kinds[i], 0) ||
            !read_socket_message(pair[1]) ||
            wait_for_workers(kinds[i]))
            return -1;
        close(pair[1]);
    }
    return 0;
}

static int test_normal_update_checks_are_reaped(void)
{
    struct timeval timeout = {1, 0};
    int i;

    if (setenv("LIBREECHO_UPDATE_FETCH", "/bin/true", 1))
        return -1;
    for (i = 0; i < 3; i++) {
        int pair[2];
        char response[512];
        size_t used = 0;
        ssize_t n;

        memset(response, 0, sizeof(response));
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
            return -1;
        if (setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof(timeout)) ||
            start_update_fetch(pair[0], "check") < 0) {
            close(pair[0]);
            close(pair[1]);
            return -1;
        }
        while (used + 1 < sizeof(response)) {
            n = read(pair[1], response + used,
                     sizeof(response) - 1 - used);
            if (n == 0)
                break;
            if (n < 0)
                return -1;
            used += (size_t)n;
        }
        close(pair[1]);
        response[used] = '\0';
        if (used == 0 || !strstr(response, "HTTP/1.1 200 OK") ||
            !strstr(response, "\"checked\":true") ||
            wait_for_workers(LE_TEST_WORKER_UPDATE_FETCH))
            return -1;
    }
    return 0;
}

static int test_registry_full_is_bounded(void)
{
    int limit = le_test_worker_limit(LE_TEST_WORKER_UPDATE_FETCH);
    int pair[2];
    int i;

    le_test_reset_worker_registry();
    for (i = 0; i < limit; i++)
        if (le_test_register_worker_pid(30000 + i,
                                        LE_TEST_WORKER_UPDATE_FETCH))
            return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
        return -1;
    if (start_update_fetch(pair[0], "check") == 0)
        return -1;
    close(pair[0]);
    close(pair[1]);
    le_test_reset_worker_registry();
    return 0;
}

static int test_fork_failure_does_not_publish_worker(void)
{
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
        return -1;
    errno = 0;
    if (le_test_spawn_noop_worker(pair[0], LE_TEST_WORKER_PCM, 1) == 0 ||
        le_test_worker_count(LE_TEST_WORKER_PCM) != 0 || errno != EAGAIN)
        return -1;
    close(pair[0]);
    close(pair[1]);
    return 0;
}

static int test_unrelated_child_status_remains_owned(void)
{
    int pair[2];
    pid_t owner;
    int status, failed;
    sigset_t blocked, previous;
    siginfo_t info;

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
        return -1;
    if (le_test_spawn_noop_worker(pair[0], LE_TEST_WORKER_UPDATE_FETCH, 0) ||
        !read_socket_message(pair[1]) ||
        wait_for_workers(LE_TEST_WORKER_UPDATE_FETCH))
        return -1;
    close(pair[1]);
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous))
        return -1;
    owner = fork();
    if (owner < 0) {
        sigprocmask(SIG_SETMASK, &previous, NULL);
        return -1;
    }
    if (owner == 0)
        _exit(7);
    /* Ensure this status is waitable before invoking the registry reaper.
     * WNOWAIT observes completion without consuming the owner's status. */
    memset(&info, 0, sizeof(info));
    failed = waitid(P_PID, (id_t)owner, &info, WEXITED | WNOWAIT) != 0;
    le_test_reap_child_workers();
    if (waitpid(owner, &status, 0) != owner ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 7)
        failed = 1;
    sigprocmask(SIG_SETMASK, &previous, NULL);
    if (!failed)
        puts("  already-exited synchronous child keeps its status: ok");
    return failed ? -1 : 0;
}

/* An inherited registry may name siblings rather than children. Model one
 * such entry with our own PID, which waitpid must report as ECHILD. */
static int test_reaper_preserves_errno(void)
{
    sigset_t blocked, previous;
    int observed, count;

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    if (sigprocmask(SIG_BLOCK, &blocked, &previous))
        return -1;
    if (le_test_register_worker_pid(getpid(), LE_TEST_WORKER_PCM)) {
        sigprocmask(SIG_SETMASK, &previous, NULL);
        return -1;
    }
    errno = EAGAIN;
    reap_child_workers(0);
    observed = errno;
    count = le_test_worker_count(LE_TEST_WORKER_PCM);
    sigprocmask(SIG_SETMASK, &previous, NULL);
    if (observed != EAGAIN || count != 0) {
        fprintf(stderr, "FAIL: reaper changed errno or retained a stale entry "
                "(errno=%d expected=%d count=%d)\n", observed, EAGAIN, count);
        return -1;
    }
    puts("  reaper preserves interrupted errno: ok");
    return 0;
}

int main(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = reap_child_workers;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &action, NULL))
        return 1;
    le_test_reset_worker_registry();
    if (test_reaper_preserves_errno() ||
        test_normal_outer_workers_are_reaped() ||
        test_normal_update_checks_are_reaped() ||
        test_registry_full_is_bounded() ||
        test_fork_failure_does_not_publish_worker() ||
        test_unrelated_child_status_remains_owned())
        return 1;
    puts("http outer-worker registry: completion, bounds, fork failure, ownership: ok");
    return 0;
}
