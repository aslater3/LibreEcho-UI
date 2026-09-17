#define _POSIX_C_SOURCE 200809L

/*
 * The service-control environment boundary.
 *
 * Every process that controls other services -- the Web daemon, the watchdog
 * recovery path and the factory-reset path -- carries its own generic
 * ARGS, DAEMON, PIDFILE and LOGFILE. Each init script resolves its settings
 * with `VAR=${VAR:-default}`, so whatever the caller happens to have set wins
 * inside the child script: a restart requested for one service starts, stops
 * or probes another service's binary, and the caller's argv is handed to an
 * unrelated daemon. Issue #249 is exactly that -- a voice-pipeline change from
 * the Web UI restarted libreecho-sttd, libreecho-ttsd, libreecho-agentd and
 * libreecho-wyomingd with the Web daemon's own command line, so they printed
 * usage and exited instead of serving their sockets.
 *
 * These names are the caller's identity, not the child's configuration, so
 * they are dropped in the forked child immediately before exec. Everything
 * else the caller carries (PATH, and the service-scoped LE_* settings a
 * script may read) is left alone: this is a boundary, not a wipe.
 */
#include "service_env.h"

#include <errno.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const char *const caller_identity[] = {
    "ARGS", "DAEMON", "PIDFILE", "LOGFILE"
};

void le_service_env_isolate(void)
{
    size_t i;

    for (i = 0; i < sizeof(caller_identity) / sizeof(caller_identity[0]); ++i)
        unsetenv(caller_identity[i]);
}

int le_service_command(const char *path, const char *const *argv)
{
    pid_t child;
    int status;

    if (!path || !path[0] || !argv)
        return -1;
    child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        le_service_env_isolate();
        execv(path, (char *const *)argv);
        _exit(127);
    }
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

/*
 * The same command for a caller that can be asked to stop while the child is
 * still running.
 *
 * The waitpid() above retries across a signal, so a SIGTERM that arrives during
 * a child's run neither stops the child nor ends the wait: for the watchdog's
 * recovery path that leaves the recovery -- a shell running another service's
 * init script -- to be reparented when its supervisor exits, after which it
 * starts a service the caller has already confirmed stopped. The reset stops
 * the supervisor first for exactly that reason.
 *
 * A stop request is therefore checked before anything is forked, and the wait
 * below polls rather than blocking. Polling is what makes the request reliable:
 * a signal that arrives before a blocking waitpid() call is handled there and
 * then, so the wait would sleep through the request -- the recovery would be
 * waited out and the service started anyway. Here the caller's flag is read
 * every tick, whatever the signal did to the wait, and the recovery is
 * terminated rather than waited out.
 *
 * The child leads its own process group, so cancelling reaches the work the
 * recovery has already started -- the dependency wait, start-stop-daemon, the
 * daemon itself -- and not only the shell holding the init script. SIGTERM
 * first, then SIGKILL for a recovery that ignores it, and the child is reaped
 * on every path: a stop that left a zombie behind would still answer kill -0
 * and the caller would read its own stop as incomplete.
 *
 * The group, not the leader, is the unit that is cancelled. A recovery shell
 * exits on the SIGTERM it is not trapping while a child it started ignores it,
 * so reaping the leader says nothing about the work: the leader can be gone
 * with the group still running, and returning there leaves the recovery --
 * reparented and alive -- doing the very thing the caller stopped it for. The
 * wait therefore covers the leader and the group separately: the leader is
 * reaped so no zombie is left, and the group is then waited on until it is
 * empty, with SIGKILL for whatever is still in it when the grace period ends.
 */
int le_service_command_cancellable(const char *path, const char *const *argv,
                                   const volatile sig_atomic_t *running)
{
    struct timespec tick = { 0, 100000000L };   /* 100 ms */
    pid_t child;
    int status;
    int polls;
    int reaped = 0;

    if (!path || !path[0] || !argv)
        return -1;
    /* The flag is already clear: the caller has been asked to stop, and forking
       now would start the very work it is quiescing, only to kill it again. */
    if (running && !*running)
        return -1;
    child = fork();
    if (child < 0)
        return -1;
    if (child == 0) {
        (void)setpgid(0, 0);
        le_service_env_isolate();
        execv(path, (char *const *)argv);
        _exit(127);
    }
    for (;;) {
        pid_t done = waitpid(child, &status, WNOHANG);

        if (done == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        if (done < 0 && errno != EINTR)
            return -1;
        if (running && !*running)
            break;
        (void)nanosleep(&tick, NULL);
    }
    /* A negative pid addresses the process group whose id is its absolute
       value, which is the group the child created for itself: the recovery and
       the work it has started. The group is never shared with this process, and
       the plain signal that follows covers the case where the group was not
       created. */
    (void)kill(-child, SIGTERM);
    (void)kill(child, SIGTERM);
    /* The leader first: a stop that leaves a zombie behind would still answer
       kill -0 and the caller would read its own stop as incomplete. */
    for (polls = 0; polls < 10 && !reaped; ++polls) {
        if (waitpid(child, &status, WNOHANG) == child)
            reaped = 1;
        else
            (void)nanosleep(&tick, NULL);
    }
    if (!reaped) {
        (void)kill(-child, SIGKILL);
        (void)kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
            continue;
        reaped = 1;
    }
    /* Then the group, which can outlive its leader: the shell that held the
       init script is reaped and the child it started -- ignoring SIGTERM --
       is still in the group. Wait for the group to empty, then SIGKILL what
       is left. The leader being gone is not the recovery being gone. */
    for (polls = 0; polls < 10; ++polls) {
        if (kill(-child, 0) < 0 && errno == ESRCH)
            break;
        (void)nanosleep(&tick, NULL);
    }
    if (kill(-child, 0) == 0 || errno != ESRCH) {
        (void)kill(-child, SIGKILL);
        (void)kill(child, SIGKILL);
        for (polls = 0; polls < 10; ++polls) {
            if (kill(-child, 0) < 0 && errno == ESRCH)
                break;
            (void)nanosleep(&tick, NULL);
        }
    }
    return -1;
}
