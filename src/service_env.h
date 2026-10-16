/*
 * The service-control environment boundary: shared by every process that runs
 * another service's init script (see service_env.c for why this exists).
 */
#ifndef LE_SERVICE_ENV_H
#define LE_SERVICE_ENV_H

#include <signal.h>

/*
 * Drop the calling service's own identity (ARGS, DAEMON, PIDFILE, LOGFILE)
 * from this process's environment. It is called in the forked child right
 * before exec so the child init script resolves its own defaults and its own
 * root-owned /etc/default file, and never the caller's command line.
 */
void le_service_env_isolate(void);

/*
 * Run `path` with `argv` (argv[0] is the process name, the array is
 * NULL-terminated) under that boundary, and wait for it. Returns 0 when the
 * child exited 0, and -1 when it could not be started, could not be waited
 * for, or exited non-zero.
 */
int le_service_command(const char *path, const char *const *argv);

/*
 * The same command, for a caller that can be asked to stop while the child is
 * still running. `running` is the caller's own still-running flag -- the one
 * its signal handler clears, like the watchdog's -- and it is read before the
 * fork and while the child runs: when it is clear the child, and the process
 * group it leads, so the work it has started goes with it, is terminated,
 * reaped, and the call reports failure. Without this a stop request neither
 * ends a wait that retries across the signal nor ends the child: for the
 * watchdog that leaves a recovery reparented and still starting the service
 * its caller has just confirmed stopped. Pass NULL for a caller with no stop
 * request. See service_env.c for why the wait polls.
 */
int le_service_command_cancellable(const char *path, const char *const *argv,
                                   const volatile sig_atomic_t *running);

/*
 * Whether any member of process group `pgid` is still able to run. A process
 * that has finished and has not been reaped -- the zombie a killed recovery's
 * descendants become once their shell is gone -- is still a member of the
 * group, so kill(-pgid, 0) reports such a group as alive and a stop that
 * trusts that answer waits out a grace period for work that is already over.
 * See service_env.c.
 */
int le_service_group_is_live(pid_t pgid);

#endif
