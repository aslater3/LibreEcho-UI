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

/* The registry entry disappearing only proves the reaper dropped the slot. A
 * reaped child must instead be unreachable: waitpid reports ECHILD. A PID that
 * is still waitable (returned, or 0 while alive) is the zombie this issue is
 * about, and a dropped-but-unreaped entry is exactly how it would survive. */
static pid_t registered_worker_pid(int kind)
{
    int i;
    for (i = 0; i < LE_MAX_CHILD_WORKERS; i++)
        if (child_worker_pids[i] > 0 && child_worker_kinds[i] == kind)
            return (pid_t)child_worker_pids[i];
    return (pid_t)-1;
}

static int worker_status_consumed(pid_t pid)
{
    int status;
    if (pid <= 0)
        return 0;
    errno = 0;
    return waitpid(pid, &status, WNOHANG) < 0 && errno == ECHILD;
}

static int read_worker_response(int fd, char *response, size_t capacity)
{
    struct timeval timeout = {1, 0};
    size_t used = 0;
    ssize_t n;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)))
        return -1;
    while (used + 1 < capacity) {
        n = read(fd, response + used, capacity - 1 - used);
        if (n == 0)
            break;
        if (n < 0)
            return -1;
        used += (size_t)n;
    }
    response[used] = '\0';
    return 0;
}

/* The issue's reproducer drives the real request path, not a stand-in. This
 * drives the real update-check worker and then proves the daemon consumed its
 * status rather than leaving it waitable. */
static int test_real_update_check_worker_is_reaped(void)
{
    int pair[2];
    char response[512];
    pid_t pid;

    if (setenv("LIBREECHO_UPDATE_FETCH", "/bin/true", 1))
        return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
        return -1;
    if (start_update_fetch(pair[0], "check") < 0) {
        close(pair[0]);
        close(pair[1]);
        return -1;
    }
    pid = registered_worker_pid(LE_TEST_WORKER_UPDATE_FETCH);
    if (read_worker_response(pair[1], response, sizeof(response))) {
        close(pair[1]);
        return -1;
    }
    close(pair[1]);
    if (pid <= 0 || !strstr(response, "HTTP/1.1 200 OK") ||
        !strstr(response, "\"checked\":true") ||
        wait_for_workers(LE_TEST_WORKER_UPDATE_FETCH) ||
        !worker_status_consumed(pid)) {
        fprintf(stderr, "FAIL: real update-check worker is still waitable "
                "(pid=%ld response=%s)\n", (long)pid, response);
        return -1;
    }
    puts("  real update-check worker is reaped, not left waiting: ok");
    return 0;
}

#define LE_TEST_STREAM_CYCLES 24

/* start_pcm_stream is the real baby-monitor audio-stream path named by the
 * issue. Repeated open/close cycles must return the process registry to
 * baseline and leave nothing waitable behind. */
static int test_real_audio_stream_workers_reap_to_baseline(void)
{
    int cycle;

    if (!access(LE_ADAPTER_WAKEWORD_SOCK, F_OK) ||
        !access(LE_ADAPTER_MIC_SOCK, F_OK)) {
        puts("  audio-stream reap cycle: skipped, a live adapter is present");
        return 0;
    }
    for (cycle = 0; cycle < LE_TEST_STREAM_CYCLES; cycle++) {
        int pair[2];
        char response[512];
        pid_t pid;

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
            return -1;
        if (start_pcm_stream(pair[0], 0) < 0) {
            close(pair[0]);
            close(pair[1]);
            return -1;
        }
        pid = registered_worker_pid(LE_TEST_WORKER_PCM);
        if (read_worker_response(pair[1], response, sizeof(response))) {
            close(pair[1]);
            return -1;
        }
        close(pair[1]);
        if (pid <= 0 || !strstr(response, "HTTP/1.1 503") ||
            wait_for_workers(LE_TEST_WORKER_PCM) ||
            le_test_worker_count(LE_TEST_WORKER_PCM) != 0 ||
            !worker_status_consumed(pid)) {
            fprintf(stderr, "FAIL: audio-stream cycle %d left its worker "
                    "unreaped (pid=%ld response=%s)\n",
                    cycle, (long)pid, response);
            return -1;
        }
    }
    printf("  audio-stream workers reap to baseline over %d cycles: ok\n",
           LE_TEST_STREAM_CYCLES);
    return 0;
}

/* After a mixed run of update and stream operations nothing may remain in the
 * bounded registry: the process count is back to baseline. */
static int test_registry_returns_to_baseline(void)
{
    int i;

    le_test_reap_child_workers();
    for (i = 0; i < LE_MAX_CHILD_WORKERS; i++)
        if (child_worker_pids[i] > 0)
            return -1;
    for (i = 0; i < CHILD_WORKER_KIND_COUNT; i++)
        if (le_test_worker_count(i) != 0)
            return -1;
    puts("  mixed update and stream run leaves no registered worker: ok");
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
        test_real_update_check_worker_is_reaped() ||
        test_real_audio_stream_workers_reap_to_baseline() ||
        test_registry_returns_to_baseline() ||
        test_registry_full_is_bounded() ||
        test_fork_failure_does_not_publish_worker() ||
        test_unrelated_child_status_remains_owned())
        return 1;
    puts("http outer-worker registry: completion, bounds, fork failure, ownership: ok");
    return 0;
}
