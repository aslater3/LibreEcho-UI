/*
 * Timer and alarm scheduling.
 *
 * The cases worth having are the ones a naive implementation gets wrong: a
 * timer that fires twice, a countdown disturbed by the wall clock moving, an
 * alarm judged against a clock that has not been set, and a ring that never
 * ends. This device has no battery-backed RTC, so "the wall clock jumps years
 * forward mid-session" is its normal boot, not an edge case.
 */

/* setenv and tzset are POSIX; the schedule itself stays C99. */
#define _POSIX_C_SOURCE 200809L

#include "adapter/timer_schedule.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

    /* The next local occurrence of a wall-clock time, in the device zone. */
    {
        long long due = 0;

        /* A fixed zone with no DST keeps the expectations exact. */
        setenv("TZ", "UTC0", 1);
        tzset();
        /* 2026-01-01 06:00 UTC: 07:00 is an hour away today. */
        assert(le_timer_next_alarm_epoch(SYNCED_EPOCH + 6 * 3600, 7, 0, &due) ==
               LE_TIMER_OK);
        assert(due == SYNCED_EPOCH + 7 * 3600);
        /* At 07:30 the same 07:00 is tomorrow, never in the past. */
        assert(le_timer_next_alarm_epoch(SYNCED_EPOCH + 7 * 3600 + 1800, 7, 0,
                                         &due) == LE_TIMER_OK);
        assert(due == SYNCED_EPOCH + 31 * 3600);
        /* Exactly the due instant counts as passed, so it moves to tomorrow. */
        assert(le_timer_next_alarm_epoch(SYNCED_EPOCH + 7 * 3600, 7, 0, &due) ==
               LE_TIMER_OK);
        assert(due == SYNCED_EPOCH + 31 * 3600);

        /* The zone is the device's: the same instant in another zone is a
           different wall-clock time. 2026-01-01 06:00 UTC is 07:00 at UTC+1. */
        setenv("TZ", "CET-1", 1);
        tzset();
        assert(le_timer_next_alarm_epoch(SYNCED_EPOCH + 6 * 3600, 7, 0, &due) ==
               LE_TIMER_OK);
        assert(due == SYNCED_EPOCH + 6 * 3600 + 24 * 3600);
        assert(le_timer_next_alarm_epoch(SYNCED_EPOCH + 6 * 3600, 8, 0, &due) ==
               LE_TIMER_OK);
        assert(due == SYNCED_EPOCH + 6 * 3600 + 3600);
        setenv("TZ", "UTC0", 1);
        tzset();

        /* An unset clock and out-of-range times are refused. */
        assert(le_timer_next_alarm_epoch(BOOT_EPOCH, 7, 0, &due) ==
               LE_TIMER_ERR_CLOCK);
        assert(le_timer_next_alarm_epoch(SYNCED_EPOCH, 24, 0, &due) ==
               LE_TIMER_ERR_RANGE);
        assert(le_timer_next_alarm_epoch(SYNCED_EPOCH, 7, 60, &due) ==
               LE_TIMER_ERR_RANGE);
    }

    /* Repeating alarms. SYNCED_EPOCH is Thursday 2026-01-01 00:00 UTC. */
    {
        const long long thursday_0700 = SYNCED_EPOCH + 25200LL;
        const long long next_monday_0700 = SYNCED_EPOCH + 4 * 86400LL + 25200LL;
        const long long friday_0700 = SYNCED_EPOCH + 86400LL + 25200LL;
        long long due = 0;
        unsigned int id = 0;
        struct le_timer_set set;
        struct le_timer *timer;

        setenv("TZ", "UTC0", 1);
        tzset();

        /* The next occurrence is the next allowed weekday at the set time. */
        assert(le_timer_next_repeat_epoch(SYNCED_EPOCH, 7, 0, 0x02U, &due) ==
               LE_TIMER_OK);
        assert(due == next_monday_0700);
        assert(le_timer_next_repeat_epoch(SYNCED_EPOCH, 7, 0, 0x3eU, &due) ==
               LE_TIMER_OK);
        assert(due == thursday_0700);
        /* Thursday 08:00 with a weekday mask rolls to Friday 07:00. */
        assert(le_timer_next_repeat_epoch(SYNCED_EPOCH + 8 * 3600LL, 7, 0, 0x3eU,
                                          &due) == LE_TIMER_OK);
        assert(due == friday_0700);
        /* Invalid input is refused rather than guessed at. */
        assert(le_timer_next_repeat_epoch(SYNCED_EPOCH, 7, 0, 0U, &due) ==
               LE_TIMER_ERR_RANGE);
        assert(le_timer_next_repeat_epoch(SYNCED_EPOCH, 7, 0, 0x80U, &due) ==
               LE_TIMER_ERR_RANGE);
        assert(le_timer_next_repeat_epoch(SYNCED_EPOCH, 24, 0, 0x7fU, &due) ==
               LE_TIMER_ERR_RANGE);
        assert(le_timer_next_repeat_epoch(BOOT_EPOCH, 7, 0, 0x7fU, &due) ==
               LE_TIMER_ERR_CLOCK);

        le_timer_set_init(&set);
        assert(le_timer_add_repeat_alarm(&set, 7, 0, 0U, "x", SYNCED_EPOCH,
                                         &id) == LE_TIMER_ERR_RANGE);
        assert(le_timer_add_repeat_alarm(&set, 7, 0, 0x3eU, "work", SYNCED_EPOCH,
                                         &id) == LE_TIMER_OK);
        timer = le_timer_find(&set, id);
        assert(timer && timer->repeat_days == 0x3eU && timer->hour == 7 &&
               timer->minute == 0 && timer->due_epoch == thursday_0700);

        /* A repeat rings at its due instant, is dismissed, and comes back on
           its next allowed day instead of being removed. */
        assert(step(&set, 1000, thursday_0700) == 1);
        assert(le_timer_ringing_count(&set) == 1);
        assert(le_timer_dismiss_at(&set, id, thursday_0700 + 10) == 1);
        timer = le_timer_find(&set, id);
        assert(timer && timer->state == LE_TIMER_STATE_PENDING);
        assert(timer->due_epoch == friday_0700);

        /* Snooze: a ringing alarm rings again after the snooze period, and
           keeps its repeat. */
        assert(step(&set, 2000, friday_0700) == 1);
        assert(le_timer_snooze_ringing(&set, friday_0700 + 5) == 1);
        timer = le_timer_find(&set, id);
        assert(timer && timer->state == LE_TIMER_STATE_PENDING);
        assert(timer->due_epoch == friday_0700 + 5 + LE_TIMER_SNOOZE_SECONDS);
        assert(timer->repeat_days == 0x3eU && timer->hour == 7);
        assert(step(&set, 3000, friday_0700 + 5 + LE_TIMER_SNOOZE_SECONDS - 1) == 0);
        assert(step(&set, 3000 + 300000,
                    friday_0700 + 5 + LE_TIMER_SNOOZE_SECONDS) == 1);
        /* Letting the ring run out also advances a repeat to the next day. */
        assert(step(&set, 3000 + 300000 + 301000,
                    friday_0700 + 5 + LE_TIMER_SNOOZE_SECONDS + 301) == 0);
        timer = le_timer_find(&set, id);
        assert(timer && timer->state == LE_TIMER_STATE_PENDING);
        /* Friday is a weekday, so the rollover after it is Monday. */
        assert(timer->due_epoch == next_monday_0700);

        /* A countdown is never snoozed, and a one-off is cleared on dismiss. */
        le_timer_set_init(&set);
        assert(le_timer_add_countdown(&set, 60, "eggs", 0, &id) == LE_TIMER_OK);
        assert(step(&set, 60000, SYNCED_EPOCH) == 1);
        assert(le_timer_snooze_ringing(&set, SYNCED_EPOCH) == 0);
        assert(le_timer_ringing_count(&set) == 1);
        assert(le_timer_dismiss_at(&set, id, SYNCED_EPOCH) == 1);
        assert(le_timer_find(&set, id) == NULL);
        assert(le_timer_add_alarm(&set, SYNCED_EPOCH + 60, "once", SYNCED_EPOCH,
                                  &id) == LE_TIMER_OK);
        assert(step(&set, 1000, SYNCED_EPOCH + 60) == 1);
        assert(le_timer_dismiss_all_at(&set, SYNCED_EPOCH + 61) == 1);
        assert(le_timer_active_count(&set) == 0);

        /* An alarm whose repeat expired while the device was down is advanced
           to its next occurrence, not reported as missed. */
        le_timer_set_init(&set);
        assert(le_timer_restore_repeat_alarm(&set, 7, SYNCED_EPOCH - 2 * 86400LL,
                                             7, 0, 0x7fU, "daily",
                                             SYNCED_EPOCH + 12 * 3600LL, 1000,
                                             &id) == LE_TIMER_OK);
        assert(id == 7 && set.missed == 0);
        timer = le_timer_find(&set, id);
        assert(timer && timer->due_epoch == friday_0700);
    }

    /* Daylight saving: an alarm set for 07:00 keeps 07:00 across the change,
       rather than drifting by an hour. Europe/London moves forward on
       2026-03-29 and back on 2026-10-25. */
    {
        struct tm when;
        long long now, due;
        time_t check;
        struct tm local;

        setenv("TZ", "Europe/London", 1);
        tzset();

        memset(&when, 0, sizeof(when));
        when.tm_year = 2026 - 1900;
        when.tm_mon = 2;
        when.tm_mday = 28;
        when.tm_hour = 12;
        when.tm_isdst = -1;
        now = (long long)mktime(&when);
        assert(le_timer_next_repeat_epoch(now, 7, 0, 0x7fU, &due) == LE_TIMER_OK);
        check = (time_t)due;
        assert(localtime_r(&check, &local));
        assert(local.tm_mday == 29 && local.tm_hour == 7 && local.tm_min == 0);
        /* Noon GMT to 07:00 BST the next day is eighteen hours, not nineteen. */
        assert(due - now == 18LL * 3600LL);

        memset(&when, 0, sizeof(when));
        when.tm_year = 2026 - 1900;
        when.tm_mon = 9;
        when.tm_mday = 24;
        when.tm_hour = 12;
        when.tm_isdst = -1;
        now = (long long)mktime(&when);
        assert(le_timer_next_repeat_epoch(now, 7, 0, 0x7fU, &due) == LE_TIMER_OK);
        check = (time_t)due;
        assert(localtime_r(&check, &local));
        assert(local.tm_mday == 25 && local.tm_hour == 7 && local.tm_min == 0);
        /* Noon BST is 11:00 UTC; 07:00 GMT the next day is 07:00 UTC: twenty hours. */
        assert(due - now == 20LL * 3600LL);

        /* A time that does not exist on the spring-forward day (01:30) still
           resolves to one instant within the day rather than failing. */
        memset(&when, 0, sizeof(when));
        when.tm_year = 2026 - 1900;
        when.tm_mon = 2;
        when.tm_mday = 28;
        when.tm_hour = 12;
        when.tm_isdst = -1;
        now = (long long)mktime(&when);
        assert(le_timer_next_repeat_epoch(now, 1, 30, 0x7fU, &due) == LE_TIMER_OK);
        assert(due > now && due < now + 2LL * 86400LL);

        setenv("TZ", "UTC0", 1);
        tzset();
    }

    printf("timer schedule: ok\n");
    return 0;
}
