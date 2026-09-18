/*
 * Watchdog restart policy.
 *
 * These cases are mostly about restraint rather than detection. Noticing a
 * dead service is easy; the failure mode worth testing is a watchdog that
 * restarts something forever, hides the original fault and keeps the device
 * busy doing it.
 */

#include "adapter/watchdog_policy.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

static void step_expect(struct le_watchdog_service *s, int healthy,
                        long long now, enum le_watchdog_action want,
                        const char *what)
{
    enum le_watchdog_action got = le_watchdog_step(s, healthy, now);

    if (got != want) {
        fprintf(stderr, "watchdog policy: %s (wanted %d, got %d)\n",
                what, (int)want, (int)got);
        _exit(1);
    }
    if (got == LE_WATCHDOG_RESTART)
        le_watchdog_restarted(s, now);
}

int main(void)
{
    /* A healthy service is left alone. */
    {
        struct le_watchdog_service s;

        le_watchdog_service_init(&s, 0);
        step_expect(&s, 1, 1000, LE_WATCHDOG_NONE, "healthy service touched");
        step_expect(&s, 1, 2000, LE_WATCHDOG_NONE, "healthy service touched");
    }

    /*
     * One failed probe is not enough. Probes race service restarts, socket
     * backlogs and momentary load; acting on a single miss would restart
     * healthy services.
     */
    {
        struct le_watchdog_service s;

        le_watchdog_service_init(&s, 0);
        step_expect(&s, 0, 1000, LE_WATCHDOG_NONE, "restarted on one failure");
        step_expect(&s, 1, 2000, LE_WATCHDOG_NONE, "recovery not accepted");
        /* and the single failure is forgotten, so it takes two again */
        step_expect(&s, 0, 3000, LE_WATCHDOG_NONE,
                    "an old failure still counted after recovery");
        step_expect(&s, 0, 4000, LE_WATCHDOG_RESTART, "two failures did not restart");
    }

    /* Backoff grows, so a service that keeps failing is not hammered. */
    {
        struct le_watchdog_service s;
        long long t = 0;

        le_watchdog_service_init(&s, 0);
        step_expect(&s, 0, t += 1000, LE_WATCHDOG_NONE, "first failure");
        step_expect(&s, 0, t += 1000, LE_WATCHDOG_RESTART, "second failure");

        /* immediately after a restart, nothing more happens */
        step_expect(&s, 0, t += 100, LE_WATCHDOG_NONE, "restarted inside backoff");
        step_expect(&s, 0, t += 100, LE_WATCHDOG_NONE, "restarted inside backoff");

        /* once the backoff has elapsed, it tries again */
        t += LE_WATCHDOG_BACKOFF_START_MS;
        step_expect(&s, 0, t, LE_WATCHDOG_RESTART, "did not retry after backoff");

        /* and the wait has grown */
        assert(s.backoff_ms > LE_WATCHDOG_BACKOFF_START_MS);
    }

    /*
     * A service that cannot be fixed is abandoned rather than restarted
     * forever. This is the case that matters: a bad config restarts instantly
     * every time, and without this the device spends the rest of its uptime
     * in a restart loop.
     */
    {
        struct le_watchdog_service s;
        long long t = 0;
        unsigned int restarts = 0;
        int gave_up = 0;
        int i;

        le_watchdog_service_init(&s, 0);
        for (i = 0; i < 400; ++i) {
            enum le_watchdog_action a = le_watchdog_step(&s, 0, t);

            if (a == LE_WATCHDOG_RESTART) {
                le_watchdog_restarted(&s, t);
                ++restarts;
            } else if (a == LE_WATCHDOG_GIVE_UP) {
                gave_up = 1;
                break;
            }
            t += 1000;
        }
        assert(gave_up);
        assert(restarts <= LE_WATCHDOG_STORM_RESTARTS);
        /* once given up, it stays given up rather than resuming quietly */
        assert(le_watchdog_step(&s, 0, t + 1000) == LE_WATCHDOG_GIVE_UP);
    }

    /*
     * A service that fails rarely but recovers properly must not accumulate
     * its way to being abandoned. A device up for weeks would otherwise give
     * up on something that was working.
     */
    {
        struct le_watchdog_service s;
        long long t = 0;
        int round;

        le_watchdog_service_init(&s, 0);
        for (round = 0; round < 10; ++round) {
            step_expect(&s, 0, t += 1000, LE_WATCHDOG_NONE, "first failure");
            step_expect(&s, 0, t += 1000, LE_WATCHDOG_RESTART, "second failure");
            /* then it stays healthy for a long time */
            t += LE_WATCHDOG_HEALTHY_RESET_MS + 1000;
            step_expect(&s, 1, t, LE_WATCHDOG_NONE, "healthy probe acted on");
            step_expect(&s, 1, t += 1000, LE_WATCHDOG_NONE, "healthy probe acted on");
            assert(!s.given_up);
        }
    }

    /*
     * Brief health between restarts must not forgive the storm counter. A
     * service that dies, gets restarted, answers for a few seconds and dies
     * again is the shape that would otherwise be restarted forever: each
     * short recovery would reset the count and the guard would never trip.
     * Only sustained health (LE_WATCHDOG_HEALTHY_RESET_MS) forgives.
     */
    {
        struct le_watchdog_service s;
        long long t = 0;
        unsigned int restarts = 0;
        int gave_up = 0;
        int i;

        le_watchdog_service_init(&s, 0);
        for (i = 0; i < 200; ++i) {
            enum le_watchdog_action a;

            le_watchdog_step(&s, 0, t); t += 1000;      /* failure one */
            a = le_watchdog_step(&s, 0, t); t += 1000;  /* failure two */
            if (a == LE_WATCHDOG_GIVE_UP) { gave_up = 1; break; }
            if (a == LE_WATCHDOG_RESTART) {
                le_watchdog_restarted(&s, t);
                ++restarts;
            }
            /* it comes back, briefly -- far less than the forgiving period */
            le_watchdog_step(&s, 1, t);
            t += 5000;
            /* and enough time passes that backoff is not what stops us */
            t += LE_WATCHDOG_BACKOFF_MAX_MS + 1000;
        }
        assert(gave_up);
        assert(restarts <= LE_WATCHDOG_STORM_RESTARTS);
    }

    printf("watchdog policy: ok\n");
    return 0;
}
