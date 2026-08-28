#include "timer_schedule.h"

#include <string.h>

static struct le_timer *free_slot(struct le_timer_set *set)
{
    size_t i;

    for (i = 0; i < LE_TIMER_MAX; ++i)
        if (set->timers[i].state == LE_TIMER_STATE_FREE)
            return &set->timers[i];
    return NULL;
}

static void set_label(struct le_timer *timer, const char *label)
{
    if (!label || !label[0]) {
        timer->label[0] = '\0';
        return;
    }
    strncpy(timer->label, label, LE_TIMER_LABEL_MAX - 1);
    timer->label[LE_TIMER_LABEL_MAX - 1] = '\0';
}

void le_timer_set_init(struct le_timer_set *set)
{
    if (!set)
        return;
    memset(set, 0, sizeof(*set));
    /* Ids start at 1 so that 0 is never a valid id and an uninitialised
       variable cannot address a real timer. */
    set->next_id = 1;
}

int le_timer_add_countdown(struct le_timer_set *set, long long seconds,
                           const char *label, long long now_monotonic_ms,
                           unsigned int *id)
{
    struct le_timer *timer;

    if (!set)
        return LE_TIMER_ERR_NOT_FOUND;
    if (seconds < LE_TIMER_MIN_SECONDS || seconds > LE_TIMER_MAX_SECONDS)
        return LE_TIMER_ERR_RANGE;
    timer = free_slot(set);
    if (!timer)
        return LE_TIMER_ERR_FULL;

    memset(timer, 0, sizeof(*timer));
    timer->id = set->next_id++;
    timer->kind = LE_TIMER_COUNTDOWN;
    timer->state = LE_TIMER_STATE_PENDING;
    timer->due_monotonic_ms = now_monotonic_ms + seconds * 1000LL;
    set_label(timer, label);
    if (id)
        *id = timer->id;
    return LE_TIMER_OK;
}

int le_timer_add_alarm(struct le_timer_set *set, long long due_epoch,
                       const char *label, long long now_epoch,
                       unsigned int *id)
{
    struct le_timer *timer;

    if (!set)
        return LE_TIMER_ERR_NOT_FOUND;
    /* Without a credible clock every alarm is nonsense: "07:00" cannot be
       placed, and any instant compares as long past. Refusing is honest;
       accepting would mean ringing the moment NTP lands. */
    if (now_epoch < LE_TIMER_CLOCK_VALID_EPOCH)
        return LE_TIMER_ERR_CLOCK;
    if (due_epoch <= now_epoch)
        return LE_TIMER_ERR_RANGE;
    timer = free_slot(set);
    if (!timer)
        return LE_TIMER_ERR_FULL;

    memset(timer, 0, sizeof(*timer));
    timer->id = set->next_id++;
    timer->kind = LE_TIMER_ALARM;
    timer->state = LE_TIMER_STATE_PENDING;
    timer->due_epoch = due_epoch;
    set_label(timer, label);
    if (id)
        *id = timer->id;
    return LE_TIMER_OK;
}

struct le_timer *le_timer_find(struct le_timer_set *set, unsigned int id)
{
    size_t i;

    if (!set || !id)
        return NULL;
    for (i = 0; i < LE_TIMER_MAX; ++i)
        if (set->timers[i].state != LE_TIMER_STATE_FREE &&
            set->timers[i].id == id)
            return &set->timers[i];
    return NULL;
}

int le_timer_cancel(struct le_timer_set *set, unsigned int id)
{
    struct le_timer *timer = le_timer_find(set, id);

    if (!timer)
        return LE_TIMER_ERR_NOT_FOUND;
    memset(timer, 0, sizeof(*timer));
    return LE_TIMER_OK;
}

int le_timer_dismiss(struct le_timer_set *set, unsigned int id)
{
    struct le_timer *timer = le_timer_find(set, id);

    if (!timer || timer->state != LE_TIMER_STATE_RINGING)
        return 0;
    memset(timer, 0, sizeof(*timer));
    return 1;
}

int le_timer_dismiss_all(struct le_timer_set *set)
{
    size_t i;
    int stopped = 0;

    if (!set)
        return 0;
    for (i = 0; i < LE_TIMER_MAX; ++i) {
        if (set->timers[i].state != LE_TIMER_STATE_RINGING)
            continue;
        memset(&set->timers[i], 0, sizeof(set->timers[i]));
        ++stopped;
    }
    return stopped;
}

static int countdown_due(const struct le_timer *timer, long long now_ms)
{
    return now_ms >= timer->due_monotonic_ms;
}

static int alarm_due(const struct le_timer *timer, long long now_epoch)
{
    return now_epoch >= timer->due_epoch;
}

/*
 * A timer this late was not "just missed" -- the device was down, asleep, or
 * had no clock. Firing it now would ring at the wrong time of day for a
 * request made about a different one.
 */
static int too_late(const struct le_timer *timer, long long now_ms,
                    long long now_epoch)
{
    long long late;

    if (timer->kind == LE_TIMER_COUNTDOWN)
        late = (now_ms - timer->due_monotonic_ms) / 1000LL;
    else
        late = now_epoch - timer->due_epoch;
    return late > LE_TIMER_MISS_GRACE_SECONDS;
}

int le_timer_step(struct le_timer_set *set, long long now_monotonic_ms,
                  long long now_epoch, unsigned int *fired, size_t fired_max)
{
    size_t i;
    int count = 0;
    int clock_valid = now_epoch >= LE_TIMER_CLOCK_VALID_EPOCH;

    if (!set)
        return 0;

    for (i = 0; i < LE_TIMER_MAX; ++i) {
        struct le_timer *timer = &set->timers[i];

        if (timer->state == LE_TIMER_STATE_RINGING) {
            if (now_monotonic_ms - timer->ring_started_ms >=
                LE_TIMER_RING_SECONDS * 1000LL)
                memset(timer, 0, sizeof(*timer));
            continue;
        }
        if (timer->state != LE_TIMER_STATE_PENDING)
            continue;

        /* An alarm is a wall-clock instant, so it cannot be judged against a
           clock that has not been set. Leaving it pending is right: it will
           be judged correctly once NTP lands. */
        if (timer->kind == LE_TIMER_ALARM && !clock_valid)
            continue;

        if (timer->kind == LE_TIMER_COUNTDOWN) {
            if (!countdown_due(timer, now_monotonic_ms))
                continue;
        } else if (!alarm_due(timer, now_epoch)) {
            continue;
        }

        if (too_late(timer, now_monotonic_ms, now_epoch)) {
            memset(timer, 0, sizeof(*timer));
            ++set->missed;
            continue;
        }

        timer->state = LE_TIMER_STATE_RINGING;
        timer->ring_started_ms = now_monotonic_ms;
        if (fired && (size_t)count < fired_max)
            fired[count] = timer->id;
        ++count;
    }
    return count;
}

int le_timer_ringing_count(const struct le_timer_set *set)
{
    size_t i;
    int count = 0;

    if (!set)
        return 0;
    for (i = 0; i < LE_TIMER_MAX; ++i)
        if (set->timers[i].state == LE_TIMER_STATE_RINGING)
            ++count;
    return count;
}

int le_timer_active_count(const struct le_timer_set *set)
{
    size_t i;
    int count = 0;

    if (!set)
        return 0;
    for (i = 0; i < LE_TIMER_MAX; ++i)
        if (set->timers[i].state != LE_TIMER_STATE_FREE)
            ++count;
    return count;
}

long long le_timer_poll_timeout_ms(const struct le_timer_set *set,
                                   long long now_monotonic_ms,
                                   long long now_epoch, long long cap_ms)
{
    size_t i;
    long long timeout = cap_ms;
    int clock_valid = now_epoch >= LE_TIMER_CLOCK_VALID_EPOCH;

    if (!set)
        return cap_ms;

    for (i = 0; i < LE_TIMER_MAX; ++i) {
        const struct le_timer *timer = &set->timers[i];
        long long remaining;

        if (timer->state == LE_TIMER_STATE_RINGING) {
            remaining = timer->ring_started_ms +
                        LE_TIMER_RING_SECONDS * 1000LL - now_monotonic_ms;
        } else if (timer->state != LE_TIMER_STATE_PENDING) {
            continue;
        } else if (timer->kind == LE_TIMER_COUNTDOWN) {
            remaining = timer->due_monotonic_ms - now_monotonic_ms;
        } else if (clock_valid) {
            remaining = (timer->due_epoch - now_epoch) * 1000LL;
        } else {
            /* The alarm cannot be judged yet, but the clock may be set at any
               moment, so do not sleep past a plausible sync. */
            continue;
        }

        if (remaining < 0)
            remaining = 0;
        if (remaining < timeout)
            timeout = remaining;
    }
    return timeout;
}
