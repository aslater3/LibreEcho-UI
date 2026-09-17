/*
 * The service-control environment boundary: shared by every process that runs
 * another service's init script (see service_env.c for why this exists).
 */
#ifndef LE_SERVICE_ENV_H
#define LE_SERVICE_ENV_H

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

#endif
