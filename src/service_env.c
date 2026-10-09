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

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/*
 * Is any member of this process group still able to run?
 *
 * kill(-pgid, 0) answers whether the group exists, not whether it can still
 * do anything. A process that has finished and has not been reaped -- a
 * zombie, which is what a recovery's descendants become once the shell that
 * was their parent has been killed -- is still a member, so a group made only
 * of zombies reads as alive and a caller waiting on that answer waits out a
 * grace period for work that is already over and cannot be killed. On a host
 * whose PID 1 does not promptly reap orphans that is every stop.
 *
 * So the group is read member by member: /proc/<pid>/stat carries each
 * process's state and its process group, and a member in state Z (or X, the
 * dead state) has finished. Only a member that is still runnable counts. The
 * comm field can hold spaces and parentheses, so the fields are read from the
 * final ')' rather than by number. Without /proc the group's existence is the
 * only evidence there is, and that answer is used instead.
 */
int le_service_group_is_live(pid_t pgid)
{
    DIR *dir;
    struct dirent *entry;
    int live = 0;

    if (pgid <= 0)
        return 0;
    dir = opendir("/proc");
    if (!dir)
        return !(kill(-pgid, 0) < 0 && errno == ESRCH);
    while (!live && (entry = readdir(dir)) != NULL) {
        char path[64];
        char line[512];
        char *state;
        char *field;
        char *end;
        FILE *file;
        long pid;

        /* The pid the directory entry names, not the string itself: a name
           that is not a number is not a process. */
        pid = strtol(entry->d_name, &end, 10);
        if (end == entry->d_name || *end != '\0' || pid <= 0)
            continue;
        snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        file = fopen(path, "r");
        if (!file)
            continue;               /* it exited while the directory was read */
        if (!fgets(line, sizeof(line), file)) {
            fclose(file);
            continue;
        }
        fclose(file);
        state = strrchr(line, ')');
        if (!state || state[1] != ' ')
            continue;
        state += 2;
        if (*state == 'Z' || *state == 'X')
            continue;               /* finished, only waiting to be reaped */
        field = strchr(state, ' ');          /* space before ppid */
        if (!field)
            continue;
        field = strchr(field + 1, ' ');      /* space before pgrp */
        if (!field)
            continue;
        if (strtol(field + 1, NULL, 10) == (long)pgid)
            live = 1;
    }
    closedir(dir);
    return live;
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
 * reaped so no zombie is left, and the group is then waited on until it has
 * no runnable member -- a member that has finished cannot be killed and must
 * not be waited on -- with SIGKILL for whatever is still running in it when
 * the grace period ends (see le_service_group_is_live).
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
       is still in the group. Wait for the group to have no runnable member,
       then SIGKILL what is left. A member that has already finished is not
       work: it is a zombie waiting to be reaped, and waiting on that would
       spend the grace period and the SIGKILL on nothing. */
    for (polls = 0; polls < 10 && le_service_group_is_live(child); ++polls)
        (void)nanosleep(&tick, NULL);
    if (le_service_group_is_live(child)) {
        (void)kill(-child, SIGKILL);
        (void)kill(child, SIGKILL);
        for (polls = 0; polls < 10 && le_service_group_is_live(child); ++polls)
            (void)nanosleep(&tick, NULL);
    }
    return -1;
}
