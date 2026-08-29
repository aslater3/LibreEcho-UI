#ifndef LE_TIMER_SCHEDULE_H
#define LE_TIMER_SCHEDULE_H

/*
 * Timers and alarms.
 *
 * Two kinds, and they do not share a clock. A countdown ("ten minutes from
 * now") is a duration, so it runs on the monotonic clock and is unaffected by
 * the wall clock moving. An alarm ("07:00") is a wall-clock instant, so it
 * runs on the wall clock and has to survive that clock being wrong.
 *
 * That split is not academic on this device. It has no battery-backed RTC:
 * every boot starts near the epoch and jumps years forward the moment NTP
 * lands. A countdown started before that sync would fire instantly if it were
 * stored as a wall-clock instant, and an alarm evaluated before the sync would
 * fire for every past occurrence at once. Hence: countdowns never look at the
 * wall clock, and alarms are not evaluated at all until it is credible.
 */

#include <stddef.h>

#define LE_TIMER_MAX 16
#define LE_TIMER_LABEL_MAX 48

/* A countdown shorter than a second is a mistake, not a request. The upper
   bound is a week: past that a countdown is really an alarm, and keeping it
   bounded keeps a typo from parking an entry forever. */
#define LE_TIMER_MIN_SECONDS 1
#define LE_TIMER_MAX_SECONDS (7 * 24 * 3600)

/* Ringing stops on its own. A device ringing into an empty house until its
   battery-less clock rolls over helps nobody, and the entry has to be cleared
   or it rings again on the next tick. */
#define LE_TIMER_RING_SECONDS 300

/* A timer that came due while the device was down still fires, but only if it
   is barely late. Waking the room at 03:00 for a countdown that expired at
   22:00 is worse than missing it, so anything older is dropped and reported
   as missed rather than rung. */
#define LE_TIMER_MISS_GRACE_SECONDS 60

/* Any wall clock earlier than 2020-01-01 is the boot default, not a time. */
#define LE_TIMER_CLOCK_VALID_EPOCH 1577836800LL

enum le_timer_kind {
    LE_TIMER_COUNTDOWN,
    LE_TIMER_ALARM
};

enum le_timer_state {
    LE_TIMER_STATE_FREE = 0,
    LE_TIMER_STATE_PENDING,
    LE_TIMER_STATE_RINGING
};

enum le_timer_result {
    LE_TIMER_OK = 0,
    LE_TIMER_ERR_FULL = -1,
    LE_TIMER_ERR_RANGE = -2,
    LE_TIMER_ERR_NOT_FOUND = -3,
    LE_TIMER_ERR_CLOCK = -4
};

struct le_timer {
    unsigned int id;
    enum le_timer_kind kind;
    enum le_timer_state state;
    /* Countdowns: monotonic milliseconds. Alarms: wall-clock seconds. Only
       the field matching the kind is meaningful. */
    long long due_monotonic_ms;
    long long due_epoch;
    long long ring_started_ms;
    char label[LE_TIMER_LABEL_MAX];
};

struct le_timer_set {
    struct le_timer timers[LE_TIMER_MAX];
    unsigned int next_id;
    /* Timers that came due while the device was down or the clock was wrong.
       Counted rather than rung, so the condition is visible in status. */
    unsigned int missed;
};

void le_timer_set_init(struct le_timer_set *set);

/*
 * Add a countdown of `seconds` from now. `label` may be NULL.
 * Returns LE_TIMER_OK and sets *id, or an error.
 */
int le_timer_add_countdown(struct le_timer_set *set, long long seconds,
                           const char *label, long long now_monotonic_ms,
                           unsigned int *id);

/*
 * Add an alarm at a wall-clock instant. Refused when the clock is not yet
 * credible, or when the instant is in the past: both would fire immediately,
 * which is never what was asked for.
 */
int le_timer_add_alarm(struct le_timer_set *set, long long due_epoch,
                       const char *label, long long now_epoch,
                       unsigned int *id);

/* Restore a persisted wall-clock due time after the clock becomes valid. A
 * due time may be slightly overdue so le_timer_step() can apply the common
 * miss grace, but stale records are rejected. */
int le_timer_restore_countdown(struct le_timer_set *set, long long due_epoch,
                               const char *label, long long now_epoch,
                               long long now_monotonic_ms, unsigned int *id);
int le_timer_restore_alarm(struct le_timer_set *set, long long due_epoch,
                           const char *label, long long now_epoch,
                           long long now_monotonic_ms, unsigned int *id);
int le_timer_restore_countdown_with_id(struct le_timer_set *set,
                                       unsigned int restore_id,
                                       long long due_epoch, const char *label,
                                       long long now_epoch,
                                       long long now_monotonic_ms,
                                       unsigned int *id);
int le_timer_restore_alarm_with_id(struct le_timer_set *set,
                                   unsigned int restore_id, long long due_epoch,
                                   const char *label, long long now_epoch,
                                   long long now_monotonic_ms,
                                   unsigned int *id);

int le_timer_cancel(struct le_timer_set *set, unsigned int id);

/* Stop a ringing timer and clear it. Returns how many were stopped. */
int le_timer_dismiss(struct le_timer_set *set, unsigned int id);
int le_timer_dismiss_all(struct le_timer_set *set);

/*
 * Advance the schedule. Newly due timers move to ringing and their ids are
 * written to `fired`; rings that have run their course are cleared.
 * Returns the number of ids written.
 */
int le_timer_step(struct le_timer_set *set, long long now_monotonic_ms,
                  long long now_epoch, unsigned int *fired, size_t fired_max);

int le_timer_ringing_count(const struct le_timer_set *set);
int le_timer_active_count(const struct le_timer_set *set);
struct le_timer *le_timer_find(struct le_timer_set *set, unsigned int id);

/*
 * Milliseconds until the next thing this schedule needs to do, capped at
 * `cap_ms`. The caller polls; without this it would either spin or oversleep
 * past a due timer.
 */
long long le_timer_poll_timeout_ms(const struct le_timer_set *set,
                                   long long now_monotonic_ms,
                                   long long now_epoch, long long cap_ms);

#endif
