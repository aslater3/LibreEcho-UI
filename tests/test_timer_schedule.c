/*
 * Timer and alarm scheduling.
 *
 * The cases worth having are the ones a naive implementation gets wrong: a
 * timer that fires twice, a countdown disturbed by the wall clock moving, an
 * alarm judged against a clock that has not been set, and a ring that never
 * ends. This device has no battery-backed RTC, so "the wall clock jumps years
 * forward mid-session" is its normal boot, not an edge case.
 */

#include "adapter/timer_schedule.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BOOT_EPOCH 10LL              /* what the clock reads before NTP */
#define SYNCED_EPOCH 1767225600LL    /* 2026-01-01, after NTP lands */

static int step(struct le_timer_set *set, long long ms, long long epoch)
{
    unsigned int fired[LE_TIMER_MAX];

    return le_timer_step(set, ms, epoch, fired, LE_TIMER_MAX);
}

int main(void)
{
    /* A countdown fires once, at its due time, and not again. */
    {
        struct le_timer_set set;
        unsigned int id = 0, fired[LE_TIMER_MAX];

        le_timer_set_init(&set);
        assert(le_timer_add_countdown(&set, 60, "pasta", 1000, &id) ==
               LE_TIMER_OK);
        assert(id != 0);

        assert(step(&set, 30000, SYNCED_EPOCH) == 0);
        assert(le_timer_step(&set, 61000, SYNCED_EPOCH, fired,
                             LE_TIMER_MAX) == 1);
        assert(fired[0] == id);
        assert(le_timer_ringing_count(&set) == 1);

        /* The classic bug: still due, so it fires on every tick. */
        assert(step(&set, 61500, SYNCED_EPOCH) == 0);
        assert(step(&set, 62000, SYNCED_EPOCH) == 0);
        assert(le_timer_ringing_count(&set) == 1);
    }

    /*
     * A countdown must not care what the wall clock does. On this device the
     * clock leaps from near zero to the real date the moment NTP lands, which
     * is routinely in the middle of a five-minute timer.
     */
    {
        struct le_timer_set set;
        unsigned int id = 0;

        le_timer_set_init(&set);
        assert(le_timer_add_countdown(&set, 300, NULL, 0, &id) ==
               LE_TIMER_OK);

        /* Clock jumps years forward one second in. */
        assert(step(&set, 1000, SYNCED_EPOCH) == 0);
        assert(le_timer_ringing_count(&set) == 0);
        /* And back again, which a bad sync can also do. */
        assert(step(&set, 2000, BOOT_EPOCH) == 0);
        assert(le_timer_ringing_count(&set) == 0);

        /* It still fires exactly when its duration is up. */
        assert(step(&set, 300000, SYNCED_EPOCH) == 1);
    }

    /*
     * An alarm is a wall-clock instant, so it cannot be created or judged
     * before the clock is credible. Judging it early would compare against a
     * clock reading a few seconds past the epoch, making every alarm look
     * decades overdue.
     */
    {
        struct le_timer_set set;
        unsigned int id = 0;

        le_timer_set_init(&set);
        assert(le_timer_add_alarm(&set, SYNCED_EPOCH + 3600, "wake",
                                  BOOT_EPOCH, &id) == LE_TIMER_ERR_CLOCK);
        assert(le_timer_active_count(&set) == 0);

        assert(le_timer_add_alarm(&set, SYNCED_EPOCH + 3600, "wake",
                                  SYNCED_EPOCH, &id) == LE_TIMER_OK);
        /* The clock goes backwards -- a resync, or a reboot before NTP. The
           alarm must sit still rather than fire or be dropped as missed. */
        assert(step(&set, 1000, BOOT_EPOCH) == 0);
        assert(le_timer_active_count(&set) == 1);
        assert(le_timer_ringing_count(&set) == 0);

        assert(step(&set, 2000, SYNCED_EPOCH + 3600) == 1);
    }

    /* An alarm in the past is a mistake, not an instruction to ring now. */
    {
        struct le_timer_set set;
        unsigned int id = 0;

        le_timer_set_init(&set);
        assert(le_timer_add_alarm(&set, SYNCED_EPOCH - 1, NULL, SYNCED_EPOCH,
                                  &id) == LE_TIMER_ERR_RANGE);
        assert(le_timer_active_count(&set) == 0);
    }

    /*
     * A timer that came due long ago is dropped rather than rung. The device
     * was off or asleep; ringing a 22:00 pasta timer at 03:00 is worse than
     * missing it, and it has to be visible rather than silent.
     */
    {
        struct le_timer_set set;
        unsigned int id = 0;

        le_timer_set_init(&set);
        assert(le_timer_add_countdown(&set, 60, NULL, 0, &id) == LE_TIMER_OK);
        /* Five hours later. */
        assert(step(&set, 5LL * 3600 * 1000, SYNCED_EPOCH) == 0);
        assert(le_timer_ringing_count(&set) == 0);
        assert(le_timer_active_count(&set) == 0);
        assert(set.missed == 1);

        /* Just barely late still rings: that is a real timer, not a stale one. */
        le_timer_set_init(&set);
        assert(le_timer_add_countdown(&set, 60, NULL, 0, &id) == LE_TIMER_OK);
        assert(step(&set, 60000 + 5000, SYNCED_EPOCH) == 1);
    }

    /* Ringing stops on its own, and the entry goes with it. */
    {
        struct le_timer_set set;
        unsigned int id = 0;

        le_timer_set_init(&set);
        assert(le_timer_add_countdown(&set, 1, NULL, 0, &id) == LE_TIMER_OK);
        assert(step(&set, 1000, SYNCED_EPOCH) == 1);
        assert(le_timer_ringing_count(&set) == 1);

        assert(step(&set, 1000 + LE_TIMER_RING_SECONDS * 1000 - 1,
                    SYNCED_EPOCH) == 0);
        assert(le_timer_ringing_count(&set) == 1);

        assert(step(&set, 1000 + LE_TIMER_RING_SECONDS * 1000,
                    SYNCED_EPOCH) == 0);
        assert(le_timer_ringing_count(&set) == 0);
        /* Cleared, not left pending -- otherwise it rings again next tick. */
        assert(le_timer_active_count(&set) == 0);
    }

    /* Dismiss stops a ringing timer; cancel removes one that has not rung. */
    {
        struct le_timer_set set;
        unsigned int ringing = 0, pending = 0;

        le_timer_set_init(&set);
        assert(le_timer_add_countdown(&set, 1, NULL, 0, &ringing) ==
               LE_TIMER_OK);
        assert(le_timer_add_countdown(&set, 600, NULL, 0, &pending) ==
               LE_TIMER_OK);
        assert(step(&set, 1000, SYNCED_EPOCH) == 1);
        assert(le_timer_cancel(&set, ringing) == LE_TIMER_ERR_NOT_FOUND);
        assert(le_timer_ringing_count(&set) == 1);

        /* Dismiss is for what is ringing. It must not silently cancel a timer
           that has not gone off -- "stop" during a ring means stop the noise,
           not throw away the other timer. */
        assert(le_timer_dismiss(&set, pending) == 0);
        assert(le_timer_active_count(&set) == 2);

        assert(le_timer_dismiss(&set, ringing) == 1);
        assert(le_timer_ringing_count(&set) == 0);
        assert(le_timer_active_count(&set) == 1);

        assert(le_timer_cancel(&set, pending) == LE_TIMER_OK);
        assert(le_timer_cancel(&set, pending) == LE_TIMER_ERR_NOT_FOUND);
        assert(le_timer_active_count(&set) == 0);
    }

    /* Dismiss-all is what "stop" reaches: every ring, nothing pending. */
    {
        struct le_timer_set set;
        unsigned int id = 0;

        le_timer_set_init(&set);
        assert(le_timer_add_countdown(&set, 1, NULL, 0, &id) == LE_TIMER_OK);
        assert(le_timer_add_countdown(&set, 1, NULL, 0, &id) == LE_TIMER_OK);
        assert(le_timer_add_countdown(&set, 600, NULL, 0, &id) ==
               LE_TIMER_OK);
        assert(step(&set, 1000, SYNCED_EPOCH) == 2);
        assert(le_timer_dismiss_all(&set) == 2);
        assert(le_timer_active_count(&set) == 1);
    }

    /* Bounds, and a full schedule that refuses rather than overwrites. */
    {
        struct le_timer_set set;
        unsigned int id = 0;
        int i;

        le_timer_set_init(&set);
        assert(le_timer_add_countdown(&set, 0, NULL, 0, &id) ==
               LE_TIMER_ERR_RANGE);
        assert(le_timer_add_countdown(&set, LE_TIMER_MAX_SECONDS + 1, NULL, 0,
                                      &id) == LE_TIMER_ERR_RANGE);

        for (i = 0; i < LE_TIMER_MAX; ++i)
            assert(le_timer_add_countdown(&set, 600, NULL, 0, &id) ==
                   LE_TIMER_OK);
        assert(le_timer_add_countdown(&set, 600, NULL, 0, &id) ==
               LE_TIMER_ERR_FULL);
        assert(le_timer_active_count(&set) == LE_TIMER_MAX);
    }

    /* Ids are not reused while a timer with that id is alive. */
    {
        struct le_timer_set set;
        unsigned int first = 0, second = 0;

        le_timer_set_init(&set);
        assert(le_timer_add_countdown(&set, 600, NULL, 0, &first) ==
               LE_TIMER_OK);
        assert(le_timer_cancel(&set, first) == LE_TIMER_OK);
        assert(le_timer_add_countdown(&set, 600, NULL, 0, &second) ==
               LE_TIMER_OK);
        /* The freed slot is reused, but the id is not: a stale "cancel 1"
           from a UI that has not refreshed must not hit the new timer. */
        assert(second != first);
        assert(le_timer_cancel(&set, first) == LE_TIMER_ERR_NOT_FOUND);
    }

    /* A long label is truncated, not overflowed. */
    {
        struct le_timer_set set;
        unsigned int id = 0;
        char label[LE_TIMER_LABEL_MAX * 2];
        struct le_timer *timer;

        memset(label, 'x', sizeof(label) - 1);
        label[sizeof(label) - 1] = '\0';
        le_timer_set_init(&set);
        assert(le_timer_add_countdown(&set, 600, label, 0, &id) ==
               LE_TIMER_OK);
        timer = le_timer_find(&set, id);
        assert(timer != NULL);
        assert(strlen(timer->label) == LE_TIMER_LABEL_MAX - 1);
    }

    /*
     * The poll timeout has to land on the next due timer. Sleeping past it
     * means a timer that rings late by however long the caller's cap is.
     */
    {
        struct le_timer_set set;
        unsigned int id = 0;

        le_timer_set_init(&set);
        assert(le_timer_poll_timeout_ms(&set, 0, SYNCED_EPOCH, 60000) ==
               60000);

        assert(le_timer_add_countdown(&set, 10, NULL, 0, &id) == LE_TIMER_OK);
        assert(le_timer_poll_timeout_ms(&set, 0, SYNCED_EPOCH, 60000) ==
               10000);
        assert(le_timer_poll_timeout_ms(&set, 9000, SYNCED_EPOCH, 60000) ==
               1000);
        /* Already due: return immediately rather than a negative sleep. */
        assert(le_timer_poll_timeout_ms(&set, 11000, SYNCED_EPOCH, 60000) ==
               0);
        /* Never longer than the caller's cap. */
        assert(le_timer_poll_timeout_ms(&set, 0, SYNCED_EPOCH, 5000) == 5000);
    }

    printf("timer schedule: ok\n");
    return 0;
}
