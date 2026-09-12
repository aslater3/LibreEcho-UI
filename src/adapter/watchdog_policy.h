#ifndef LE_WATCHDOG_POLICY_H
#define LE_WATCHDOG_POLICY_H

/*
 * When to restart a supervised service, kept separate from the code that does
 * the restarting so the decisions can be tested without processes or sockets.
 *
 * The hard part of a watchdog is not noticing a dead service, it is not making
 * things worse. A service that dies because its configuration is wrong will
 * die again immediately, and a watchdog without restraint turns that into an
 * endless restart loop that buries the original error and keeps the device
 * busy. So the policy has three guards: it waits for a service to fail twice
 * before believing it, it backs off between attempts, and it gives up rather
 * than restarting forever.
 */

#define LE_WATCHDOG_FAILURES_BEFORE_RESTART 2U

/* 1, 2, 4, 8, then 30 s. Capped so a service that recovers slowly is still
   retried at a sensible cadence rather than drifting into hours. */
#define LE_WATCHDOG_BACKOFF_START_MS 1000LL
#define LE_WATCHDOG_BACKOFF_MAX_MS 30000LL

/* Give up after this many restarts inside the window. A service that needs
   five restarts in five minutes is not going to be fixed by a sixth. */
#define LE_WATCHDOG_STORM_RESTARTS 5U
#define LE_WATCHDOG_STORM_WINDOW_MS 300000LL

/* A service that has stayed healthy this long has genuinely recovered, so its
   restart count is forgiven. Without this, a device up for weeks would
   eventually give up on a service that failed rarely but survived fine. */
#define LE_WATCHDOG_HEALTHY_RESET_MS 600000LL

enum le_watchdog_action {
    LE_WATCHDOG_NONE = 0,
    LE_WATCHDOG_RESTART,
    LE_WATCHDOG_GIVE_UP
};

struct le_watchdog_service {
    unsigned int failures;      /* consecutive failed probes */
    unsigned int restarts;      /* restarts inside the current window */
    long long backoff_ms;       /* current wait between attempts */
    long long next_attempt_ms;  /* earliest time a restart may be issued */
    long long window_started_ms;
    long long healthy_since_ms; /* 0 when not currently healthy */
    int given_up;
};

void le_watchdog_service_init(struct le_watchdog_service *service,
                              long long now_ms);

/*
 * Feed one probe result. `healthy` is non-zero when the service answered.
 * Returns what the caller should do now.
 */
enum le_watchdog_action le_watchdog_step(struct le_watchdog_service *service,
                                         int healthy, long long now_ms);

/* Call after a restart has actually been issued, so backoff and the storm
   counter advance only for attempts that really happened. */
void le_watchdog_restarted(struct le_watchdog_service *service,
                           long long now_ms);

#endif
