/*
 * The wait behind the supervisor's recovery path.
 *
 * watchdogd launches each recovery as an init-script child through
 * le_service_command(), and waits for it. The plain wait retries across a
 * signal, so a stop request that arrives while a recovery is running neither
 * ends the wait nor ends the recovery: the child is reparented when the
 * supervisor exits and finishes starting a service the caller has already
 * confirmed stopped. The factory reset depends on that stop being complete.
 *
 * These cases pin both halves of the repair: a stop request ends the recovery
 * that is in flight -- the shell and the work it has started -- and leaves no
 * zombie behind, a recovery that ignores the request is still killed, and an
 * interruption on its own -- no stop requested -- must let a child that is
 * making progress run to completion. The last case is the one that keeps the
 * fix from turning into "any signal ends supervision work", which would
 * abandon a healthy recovery instead of finishing it.
 *
 * The group case is the one that keeps the stop from ending at the leader: a
 * recovery shell can exit on the signal while the child it started ignores it,
 * so the shell being reaped is not the recovery being gone.
 *
 * The flag handed to the helper is the same shape as watchdogd's: it is set
 * while the caller wants the work to continue, and the handler that delivers
 * the stop request clears it. SIGALRM stands in for the SIGTERM the daemon
 * gets, and every interrupting case asserts the signal really arrived -- a case
 * that never got its signal would otherwise pass by doing nothing.
 */
#define _POSIX_C_SOURCE 200809L

#include "service_env.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;
static volatile sig_atomic_t interrupts;
static volatile sig_atomic_t request_stop;

/* watchdogd's stop(): the signal only clears the flag the caller is reading. */
static void on_alarm(int signo)
{
    (void)signo;
    ++interrupts;
    if (request_stop)
        running = 0;
}

static double now(void)
{
    struct timespec ts;

    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

/* Interrupt the recovery after `seconds`, asking it to stop or not. No
   SA_RESTART: the wait has to see the signal. */
static void arm_alarm(unsigned seconds, int stop_requested)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alarm;
    sa.sa_flags = 0;
    assert(sigemptyset(&sa.sa_mask) == 0);
    assert(sigaction(SIGALRM, &sa, NULL) == 0);
    running = 1;
    interrupts = 0;
    request_stop = stop_requested;
    alarm(seconds);
}

/* No child of this process may be left unreaped: a recovery that ends as a
   zombie still answers kill -0, which is how a stop looks complete while the
   process it launched is still there. */
static void assert_no_child_left(void)
{
    int status;

    errno = 0;
    assert(waitpid(-1, &status, WNOHANG) == -1);
    assert(errno == ECHILD);
}

/* A process counts as alive only while it really is one: a zombie has finished
   and is only waiting to be reaped. */
static int alive(pid_t pid)
{
    char path[64];
    char line[256];
    char *end;
    FILE *file;

    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    file = fopen(path, "r");
    if (!file)
        return 0;
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return 0;
    }
    fclose(file);
    /* The comm field can contain spaces and parentheses, so the state is read
       after the final ')' rather than by field number. */
    end = strrchr(line, ')');
    if (!end || end[1] != ' ')
        return 0;
    return end[2] != 'Z' && end[2] != 'X';
}

static int run_script(const char *script, volatile sig_atomic_t *flag, double *elapsed)
{
    const char *argv[] = { "/bin/sh", "-c", script, NULL };
    double begin = now();
    int result = le_service_command_cancellable("/bin/sh", argv, flag);

    *elapsed = now() - begin;
    return result;
}

/* A stop request during the recovery: the child is terminated, reaped, and the
   call reports failure so the caller does not treat a cancelled recovery as a
   successful one. */
static void test_stop_ends_the_recovery(void)
{
    double elapsed;

    arm_alarm(1, 1);
    assert(run_script("sleep 5", &running, &elapsed) == -1);
    alarm(0);
    assert(interrupts >= 1);
    assert(elapsed >= 0.5);
    assert(elapsed < 2.5);
    assert_no_child_left();
}

/* A recovery that ignores SIGTERM is still killed, and the caller's stop stays
   bounded rather than waiting the recovery out. */
static void test_stop_kills_a_recovery_that_ignores_it(void)
{
    double elapsed;

    arm_alarm(1, 1);
    assert(run_script("trap '' TERM; while :; do sleep 0.2; done", &running, &elapsed) == -1);
    alarm(0);
    assert(interrupts >= 1);
    /* SIGTERM was ignored, so this took the grace period and then SIGKILL. */
    assert(elapsed >= 1.8);
    assert(elapsed < 4.0);
    assert_no_child_left();
}

/* The recovery is not only the shell: a service init waits for its dependencies
   before it launches anything, and whatever it has already started has to go
   with it. Killing the shell alone leaves that work reparented and running,
   which is the state the stop was meant to end. */
static void test_stop_ends_the_work_the_recovery_started(void)
{
    char path[] = "/tmp/libreecho-cancel-progress.XXXXXX";
    char script[512];
    char line[64];
    double elapsed;
    FILE *file;
    long pid;
    int fd;

    fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unlink(path);
    /* The script records the background child only once it is visible in
       /proc, so an empty file means the case never happened rather than that
       the child was never there. */
    snprintf(script, sizeof(script),
             "sleep 30 & child=$!; [ -d \"/proc/$child\" ] && echo $child > %s; wait",
             path);
    arm_alarm(1, 1);
    assert(run_script(script, &running, &elapsed) == -1);
    alarm(0);
    assert(interrupts >= 1);
    assert(elapsed < 3.0);

    file = fopen(path, "r");
    assert(file != NULL);
    assert(fgets(line, sizeof(line), file) != NULL);
    fclose(file);
    pid = strtol(line, NULL, 10);
    assert(pid > 1);
    assert(!alive((pid_t)pid));
    unlink(path);
    assert_no_child_left();
}

/* An interruption with no stop requested is not a cancellation: the child is
   making progress and must run to completion, or a healthy recovery would be
   abandoned because an unrelated signal arrived. */
static void test_interruption_alone_does_not_cancel(void)
{
    double elapsed;

    arm_alarm(1, 0);
    assert(run_script("sleep 2", &running, &elapsed) == 0);
    alarm(0);
    /* The wait really was interrupted, and still saw the child through. */
    assert(interrupts >= 1);
    assert(elapsed >= 1.5);
    assert_no_child_left();
}

/* The leader is not the group. A recovery shell that does not trap a signal
   exits on the SIGTERM while the work it started ignores the same signal, so
   the shell is reaped while the group is still running. A stop that returns
   on the leader's exit leaves that work reparented and alive -- exactly the
   state the caller stopped it for -- so the group is waited on and killed
   even though the leader is already gone. */
static void test_stop_ends_the_group_after_the_leader_exits(void)
{
    char path[] = "/tmp/libreecho-cancel-group.XXXXXX";
    char script[512];
    char line[64];
    double elapsed;
    FILE *file;
    long pid;
    int fd;

    fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    unlink(path);
    /* The background child ignores SIGTERM and records itself once it is
       visible in /proc, so an empty file means the case never happened rather
       than that nothing was left behind. The shell traps the signal with an
       exit, so its own death is not the group's death. */
    snprintf(script, sizeof(script),
             "sh -c 'trap \"\" TERM; while :; do sleep 0.3; done' & child=$!; "
             "[ -d \"/proc/$child\" ] && echo $child > %s; "
             "trap 'exit 0' TERM; wait",
             path);
    arm_alarm(1, 1);
    assert(run_script(script, &running, &elapsed) == -1);
    alarm(0);
    assert(interrupts >= 1);
    /* The leader went at the stop request and the group was not waited out:
       SIGTERM grace for the group, then SIGKILL. */
    assert(elapsed < 4.0);

    file = fopen(path, "r");
    assert(file != NULL);
    assert(fgets(line, sizeof(line), file) != NULL);
    fclose(file);
    pid = strtol(line, NULL, 10);
    assert(pid > 1);
    assert(!alive((pid_t)pid));
    unlink(path);
    assert_no_child_left();
}

/* A caller that has already been asked to stop starts nothing: the recovery
   would be work it is quiescing, forked only to be killed again. */
static void test_stop_before_the_fork_starts_nothing(void)
{
    double elapsed;

    request_stop = 0;
    running = 0;
    assert(run_script("echo started > /tmp/libreecho-cancel-should-not-exist",
                      &running, &elapsed) == -1);
    assert(elapsed < 0.5);
    assert(access("/tmp/libreecho-cancel-should-not-exist", F_OK) != 0);
    assert_no_child_left();
}

/* The plain wait is unchanged: NULL and a caller that never stopped both wait
   the child out, and the exit status is still reported. */
static void test_wait_semantics_are_unchanged(void)
{
    double elapsed;

    running = 1;
    request_stop = 0;
    assert(run_script("exit 0", NULL, &elapsed) == 0);
    assert(run_script("exit 3", NULL, &elapsed) == -1);
    assert(run_script("exit 3", &running, &elapsed) == -1);
    assert_no_child_left();
}

int main(void)
{
    test_stop_ends_the_recovery();
    test_stop_kills_a_recovery_that_ignores_it();
    test_stop_ends_the_work_the_recovery_started();
    test_interruption_alone_does_not_cancel();
    test_stop_ends_the_group_after_the_leader_exits();
    test_stop_before_the_fork_starts_nothing();
    test_wait_semantics_are_unchanged();
    puts("service command cancellation: stop ends the recovery "
         "and the work it started, progress is not cancelled: ok");
    return 0;
}
