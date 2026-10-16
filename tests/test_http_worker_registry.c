#define _POSIX_C_SOURCE 200809L
#define LE_HTTP_SERVER_WORKER_TEST 1

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../src/http_server.c"

#define LE_TEST_READ_SECONDS 5
#define LE_TEST_WORKER_EXIT_MS 5000
#define LE_TEST_INTERRUPT_BURSTS 50
#define LE_TEST_INTERRUPT_LIMIT 10000
#define LE_TEST_ADAPTER_PROBE_POLLS 20
#define LE_TEST_ADAPTER_PROBE_MS 10

static void deadline_after(struct timespec *deadline, long seconds)
{
    clock_gettime(CLOCK_MONOTONIC, deadline);
    deadline->tv_sec += seconds;
}

static int deadline_expired(const struct timespec *deadline)
{
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec != deadline->tv_sec)
        return now.tv_sec > deadline->tv_sec;
    return now.tv_nsec >= deadline->tv_nsec;
}

/* A worker may be reaped while its response is being read, and a signal
 * delivered into a blocked read on a socket carrying SO_RCVTIMEO reports
 * EINTR rather than restarting, even under SA_RESTART. Only that result is
 * retried; SO_RCVTIMEO stays set, so a silent peer still fails the read at the
 * timeout, and the retries run inside a monotonic deadline so a signal storm
 * cannot make the read unbounded. */
static ssize_t read_response_bytes(int fd, void *buffer, size_t count)
{
    struct timespec deadline;
    ssize_t n;

    deadline_after(&deadline, LE_TEST_READ_SECONDS);
    for (;;) {
        n = read(fd, buffer, count);
        if (n >= 0 || errno != EINTR)
            return n;
        if (deadline_expired(&deadline))
            return -1;
    }
}

/* capacity includes the terminator: at most capacity-1 bytes are read. */
static int read_worker_response(int fd, char *response, size_t capacity)
{
    struct timeval timeout = {1, 0};
    size_t used = 0;
    ssize_t n;

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)))
        return -1;
    while (used + 1 < capacity) {
        n = read_response_bytes(fd, response + used, capacity - 1 - used);
        if (n == 0)
            break;
        if (n < 0)
            return -1;
        used += (size_t)n;
    }
    response[used] = '\0';
    return 0;
}

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

    return read_worker_response(fd, message, sizeof(message)) == 0 &&
           !strcmp(message, "ok");
}

static int start_pcm_stream_worker(int fd)
{
    return start_pcm_stream(fd, 0);
}

static int start_update_check_worker(int fd)
{
    return start_update_fetch(fd, "check");
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
    int i;

    if (setenv("LIBREECHO_UPDATE_FETCH", "/bin/true", 1))
        return -1;
    for (i = 0; i < 3; i++) {
        int pair[2];
        char response[512];

        memset(response, 0, sizeof(response));
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
            return -1;
        if (start_update_check_worker(pair[0]) < 0) {
            close(pair[0]);
            close(pair[1]);
            return -1;
        }
        if (read_worker_response(pair[1], response, sizeof(response)))
            return -1;
        close(pair[1]);
        if (response[0] == '\0' || !strstr(response, "HTTP/1.1 200 OK") ||
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

static int sigchld_block(sigset_t *previous)
{
    sigset_t blocked;

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    return sigprocmask(SIG_BLOCK, &blocked, previous);
}

static void sigchld_restore(const sigset_t *previous)
{
    sigprocmask(SIG_SETMASK, previous, NULL);
}

typedef int (*worker_start)(int fd);

/* A short-lived worker exits as soon as it has answered, and the installed
 * handler reaps it the moment it is allowed to run - clearing the registry
 * slot this test needs. Hold SIGCHLD blocked across the real startup and the
 * PID snapshot, the same window child_worker_begin() holds, so the lookup
 * cannot lose a worker that ran correctly. */
static int start_worker_and_capture_pid(worker_start start, int fd, int kind,
                                        pid_t *pid)
{
    sigset_t previous;

    if (sigchld_block(&previous))
        return -1;
    if (start(fd) < 0 || (*pid = registered_worker_pid(kind)) <= 0) {
        sigchld_restore(&previous);
        return -1;
    }
    sigchld_restore(&previous);
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
    if (start_worker_and_capture_pid(start_update_check_worker, pair[0],
                                     LE_TEST_WORKER_UPDATE_FETCH, &pid)) {
        fprintf(stderr, "FAIL: real update-check worker published no PID "
                "(errno=%d)\n", errno);
        close(pair[1]);
        return -1;
    }
    if (read_worker_response(pair[1], response, sizeof(response))) {
        close(pair[1]);
        return -1;
    }
    close(pair[1]);
    if (!strstr(response, "HTTP/1.1 200 OK") ||
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

/* The worker only enters its long-lived stream when an adapter actually accepts
 * the connection. Existence is not liveness - a node left behind by an adapter
 * that exited uncleanly would make the worker's own connect() fail - so probe
 * the connection the worker makes. The probe must not be able to hang itself:
 * it connects non-blocking and waits only briefly, and a node that does not
 * answer within that window is treated as live, so the tests skip instead of
 * risking a stall on a wedged listener. */
static int adapter_accepts_connection(const char *path)
{
    struct sockaddr_un address;
    struct pollfd ready;
    socklen_t length;
    int error, fd, i, result;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return 1;
    if (fcntl(fd, F_SETFL, O_NONBLOCK)) {
        close(fd);
        return 1;
    }
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);
    result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (!result) {
        close(fd);
        return 1;
    }
    if (errno != EINPROGRESS) {
        result = errno;
        close(fd);
        /* A full backlog is an answer, not a missing listener. */
        return result == EAGAIN || result == EWOULDBLOCK;
    }
    ready.fd = fd;
    ready.events = POLLOUT;
    for (i = 0; i < LE_TEST_ADAPTER_PROBE_POLLS; i++) {
        ready.revents = 0;
        if (poll(&ready, 1, LE_TEST_ADAPTER_PROBE_MS) > 0 && ready.revents)
            break;
    }
    if (i == LE_TEST_ADAPTER_PROBE_POLLS) {
        close(fd);
        return 1;
    }
    error = 0;
    length = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length))
        error = 0;
    close(fd);
    return error == 0;
}

static int live_audio_adapter(void)
{
    return adapter_accepts_connection(LE_ADAPTER_WAKEWORD_SOCK) ||
           adapter_accepts_connection(LE_ADAPTER_MIC_SOCK);
}

/* Proof that the snapshot window is deterministic: with SIGCHLD blocked the
 * handler cannot clear the slot even though the worker has already exited, so
 * the cycle tests cannot lose a worker that behaved correctly. After the
 * window closes, the pending signal reaps the worker and the slot empties. */
static int test_pid_snapshot_survives_worker_exit(void)
{
    struct timespec pause = {0, 1000000L};
    sigset_t previous;
    siginfo_t info;
    int pair[2];
    pid_t pid;
    int i, status, failed = 0;

    if (live_audio_adapter()) {
        puts("  PID snapshot window: skipped, a live adapter is present");
        return 0;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
        return -1;
    if (sigchld_block(&previous)) {
        close(pair[0]);
        close(pair[1]);
        return -1;
    }
    if (start_pcm_stream_worker(pair[0]) < 0 ||
        (pid = registered_worker_pid(LE_TEST_WORKER_PCM)) <= 0) {
        fprintf(stderr, "FAIL: audio-stream worker published no PID inside the "
                "blocked window (errno=%d)\n", errno);
        sigchld_restore(&previous);
        close(pair[1]);
        return -1;
    }
    /* WNOWAIT observes the exit without consuming the status, so the slot still
     * has to name the worker while SIGCHLD stays blocked. The wait is polled
     * rather than blocking, so a worker that never exits fails the test instead
     * of hanging it. */
    memset(&info, 0, sizeof(info));
    for (i = 0; i < LE_TEST_WORKER_EXIT_MS && !info.si_pid; i++) {
        if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOWAIT | WNOHANG)) {
            fprintf(stderr, "FAIL: waiting on audio-stream worker %ld failed "
                    "(errno=%d)\n", (long)pid, errno);
            failed = 1;
            break;
        }
        if (!info.si_pid)
            nanosleep(&pause, NULL);
    }
    if (!failed && info.si_pid != pid) {
        fprintf(stderr, "FAIL: audio-stream worker %ld did not exit within "
                "%d ms\n", (long)pid, LE_TEST_WORKER_EXIT_MS);
        failed = 1;
    }
    if (!failed && registered_worker_pid(LE_TEST_WORKER_PCM) != pid) {
        fprintf(stderr, "FAIL: the exited worker's slot was cleared while "
                "SIGCHLD was blocked\n");
        failed = 1;
    }
    sigchld_restore(&previous);
    if (failed) {
        /* Never leave a streaming worker behind on the failure path. */
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        close(pair[1]);
        return -1;
    }
    for (i = 0; i < 200 && !worker_status_consumed(pid); i++)
        nanosleep(&pause, NULL);
    close(pair[1]);
    if (!worker_status_consumed(pid) ||
        registered_worker_pid(LE_TEST_WORKER_PCM) != (pid_t)-1) {
        fprintf(stderr, "FAIL: the pending SIGCHLD did not reap worker %ld\n",
                (long)pid);
        return -1;
    }
    puts("  PID snapshot survives an exited worker inside the blocked "
         "window: ok");
    return 0;
}

#define LE_TEST_STREAM_CYCLES 24

/* start_pcm_stream is the real baby-monitor audio-stream path named by the
 * issue. Repeated open/close cycles must return the process registry to
 * baseline and leave nothing waitable behind. */
static int test_real_audio_stream_workers_reap_to_baseline(void)
{
    int cycle;

    if (live_audio_adapter()) {
        puts("  audio-stream reap cycle: skipped, a live adapter is present");
        return 0;
    }
    for (cycle = 0; cycle < LE_TEST_STREAM_CYCLES; cycle++) {
        int pair[2];
        char response[512];
        pid_t pid;

        if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
            return -1;
        if (start_worker_and_capture_pid(start_pcm_stream_worker, pair[0],
                                         LE_TEST_WORKER_PCM, &pid)) {
            fprintf(stderr, "FAIL: audio-stream cycle %d published no PID "
                    "(errno=%d)\n", cycle, errno);
            close(pair[1]);
            return -1;
        }
        if (read_worker_response(pair[1], response, sizeof(response))) {
            fprintf(stderr, "FAIL: audio-stream cycle %d read no response "
                    "(errno=%d)\n", cycle, errno);
            close(pair[1]);
            return -1;
        }
        close(pair[1]);
        if (!strstr(response, "HTTP/1.1 503") ||
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

/* Keeps interrupting the parent's read with SIGCHLD - the signal the daemon
 * installs reap_child_workers() for, under the same SA_RESTART - for as long as
 * the parent holds its end of the socket open, so the interruption cannot
 * depend on when the parent reaches read(). The helper drops its inherited copy
 * of the parent's end, so closing that end really ends the loop, and the loop
 * is capped as well so a stuck helper cannot hang the test. When a payload is
 * given it is written after a burst of interruptions and the helper exits,
 * closing its own end. */
static pid_t fork_interrupting_writer(int fd, int peer, const char *payload,
                                    size_t length)
{
    struct pollfd watched;
    struct timespec pause = {0, 1000000L};
    int bursts = 0, rounds = 0;
    pid_t pid = fork();

    if (pid != 0)
        return pid;
    close(peer);
    watched.fd = fd;
    watched.events = POLLIN;
    for (;;) {
        watched.revents = 0;
        poll(&watched, 1, 1);
        if (watched.revents || rounds++ > LE_TEST_INTERRUPT_LIMIT)
            break;
        kill(getppid(), SIGCHLD);
        if (length && ++bursts > LE_TEST_INTERRUPT_BURSTS) {
            if (write(fd, payload, length) != (ssize_t)length)
                _exit(1);
            _exit(0);
        }
        nanosleep(&pause, NULL);
    }
    _exit(0);
}

/* The cycle tests read a response while a worker is being reaped, so a real
 * SIGCHLD really can land inside that read. Prove both halves: the raw call
 * reports EINTR rather than restarting, and the reader retries it instead of
 * reporting a correctly reaped worker as a failure. */
static int test_interrupted_response_read_is_retried(void)
{
    struct timeval timeout = {5, 0};
    int pair[2];
    char response[8];
    pid_t helper;
    ssize_t interrupted;
    int status, failed = 0;

    /* First half: the raw call the reader has to tolerate. The helper keeps
     * delivering SIGCHLD until this end is closed, so the read is interrupted
     * however long it takes the parent to enter it. */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
        return -1;
    if (setsockopt(pair[1], SOL_SOCKET, SO_RCVTIMEO, &timeout,
                   sizeof(timeout)))
        return -1;
    helper = fork_interrupting_writer(pair[0], pair[1], NULL, 0);
    if (helper < 0)
        return -1;
    close(pair[0]);
    errno = 0;
    interrupted = read(pair[1], response, 1);
    if (!(interrupted < 0 && errno == EINTR)) {
        fprintf(stderr, "FAIL: a signal during a timed socket read did not "
                "report EINTR (%ld/%s); the retry would not be exercised\n",
                (long)interrupted, strerror(errno));
        failed = 1;
    }
    close(pair[1]);
    waitpid(helper, &status, 0);

    /* Second half: the same interruptions while the worker's response is read,
     * which still has to arrive because the reader retries. */
    memset(response, 0, sizeof(response));
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair)) {
        failed = 1;
    } else if ((helper = fork_interrupting_writer(pair[0], pair[1], "ok", 2)) < 0) {
        failed = 1;
    } else {
        close(pair[0]);
        if (read_worker_response(pair[1], response, sizeof(response)) ||
            strcmp(response, "ok")) {
            fprintf(stderr, "FAIL: an interrupted response read was reported "
                    "as a failure (response=%s errno=%d)\n",
                    response, errno);
            failed = 1;
        }
        close(pair[1]);
        waitpid(helper, &status, 0);
    }
    if (!failed)
        puts("  an interrupted response read is retried, not failed: ok");
    return failed ? -1 : 0;
}

/* Fork a child that exits 9, register it and then drop its registry entry
 * without consuming its status - the shape a reaper that dropped a slot
 * instead of reaping the child would leave behind. SIGCHLD is held blocked, so
 * the handler cannot hide that. */
static pid_t fork_dropped_worker(void)
{
    siginfo_t info;
    pid_t pid = fork();

    if (pid < 0)
        return -1;
    if (pid == 0)
        _exit(9);
    if (le_test_register_worker_pid(pid, LE_TEST_WORKER_PCM))
        return -1;
    memset(&info, 0, sizeof(info));
    if (waitid(P_PID, (id_t)pid, &info, WEXITED | WNOWAIT))
        return -1;
    le_test_reset_worker_registry();
    return pid;
}

/* The cycle tests assert that a reaped worker's status is gone. Prove that
 * assertion is not vacuous: an entry dropped without the status being consumed
 * - the defect this issue is about - is reported as unconsumed, and only a real
 * reap flips it. worker_status_consumed() itself reaps what it finds, so each
 * direction gets its own worker. */
static int test_dropped_entry_without_reap_is_detected(void)
{
    sigset_t previous;
    pid_t pid;
    int status, failed = 0;

    if (sigchld_block(&previous))
        return -1;

    pid = fork_dropped_worker();
    if (pid <= 0 || worker_status_consumed(pid)) {
        fprintf(stderr, "FAIL: the reap check cannot see a dropped-but-"
                "unreaped worker\n");
        failed = 1;
    }
    if (pid > 0)
        waitpid(pid, &status, 0);

    pid = fork_dropped_worker();
    if (pid <= 0 || waitpid(pid, &status, 0) != pid ||
        !WIFEXITED(status) || WEXITSTATUS(status) != 9 ||
        !worker_status_consumed(pid)) {
        fprintf(stderr, "FAIL: a reaped worker is not reported as consumed "
                "(pid=%ld)\n", (long)pid);
        failed = 1;
    }

    le_test_reset_worker_registry();
    sigchld_restore(&previous);
    if (!failed)
        puts("  a dropped-but-unreaped entry is still detected: ok");
    return failed ? -1 : 0;
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
        test_pid_snapshot_survives_worker_exit() ||
        test_real_audio_stream_workers_reap_to_baseline() ||
        test_interrupted_response_read_is_retried() ||
        test_registry_returns_to_baseline() ||
        test_dropped_entry_without_reap_is_detected() ||
        test_registry_full_is_bounded() ||
        test_fork_failure_does_not_publish_worker() ||
        test_unrelated_child_status_remains_owned())
        return 1;
    puts("http outer-worker registry: completion, bounds, fork failure, ownership: ok");
    return 0;
}
