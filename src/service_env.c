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
