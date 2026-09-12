#include "watchdog_policy.h"

#include <string.h>

void le_watchdog_service_init(struct le_watchdog_service *service,
                              long long now_ms)
{
    if (!service)
        return;
    memset(service, 0, sizeof(*service));
    service->backoff_ms = LE_WATCHDOG_BACKOFF_START_MS;
    service->window_started_ms = now_ms;
    service->healthy_since_ms = now_ms;
}

static void forgive_if_settled(struct le_watchdog_service *service,
                               long long now_ms)
{
    /*
     * Only a genuinely sustained recovery clears the restart count. Clearing
     * it on the first healthy probe would defeat the storm guard entirely: a
     * service that dies, restarts, answers once and dies again would look new
     * every time and could be restarted forever.
     */
    if (!service->healthy_since_ms)
        return;
    if (now_ms - service->healthy_since_ms < LE_WATCHDOG_HEALTHY_RESET_MS)
        return;
    service->restarts = 0;
    service->backoff_ms = LE_WATCHDOG_BACKOFF_START_MS;
    service->window_started_ms = now_ms;
}

enum le_watchdog_action le_watchdog_step(struct le_watchdog_service *service,
                                         int healthy, long long now_ms)
{
    if (!service)
        return LE_WATCHDOG_NONE;
    if (service->given_up)
        return LE_WATCHDOG_GIVE_UP;

    if (healthy) {
        service->failures = 0;
        if (!service->healthy_since_ms)
            service->healthy_since_ms = now_ms;
        forgive_if_settled(service, now_ms);
        return LE_WATCHDOG_NONE;
    }

    service->healthy_since_ms = 0;
    if (service->failures < LE_WATCHDOG_FAILURES_BEFORE_RESTART)
        ++service->failures;
    if (service->failures < LE_WATCHDOG_FAILURES_BEFORE_RESTART)
        return LE_WATCHDOG_NONE;

    /* The storm window is measured from the first restart in it, so a burst
       of failures inside one window cannot be laundered by the clock. */
    if (now_ms - service->window_started_ms > LE_WATCHDOG_STORM_WINDOW_MS) {
        service->window_started_ms = now_ms;
        service->restarts = 0;
    }
    if (service->restarts >= LE_WATCHDOG_STORM_RESTARTS) {
        service->given_up = 1;
        return LE_WATCHDOG_GIVE_UP;
    }
    if (now_ms < service->next_attempt_ms)
        return LE_WATCHDOG_NONE;
    return LE_WATCHDOG_RESTART;
}

void le_watchdog_restarted(struct le_watchdog_service *service,
                           long long now_ms)
{
    if (!service)
        return;
    if (!service->restarts)
        service->window_started_ms = now_ms;
    ++service->restarts;
    service->failures = 0;          /* re-earn the failures before trying again */
    service->healthy_since_ms = 0;
    service->next_attempt_ms = now_ms + service->backoff_ms;
    service->backoff_ms *= 2;
    if (service->backoff_ms > LE_WATCHDOG_BACKOFF_MAX_MS)
        service->backoff_ms = LE_WATCHDOG_BACKOFF_MAX_MS;
}
