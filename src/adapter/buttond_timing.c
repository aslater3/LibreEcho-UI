#include "buttond_timing.h"

#include <limits.h>

long long buttond_poll_timeout_ms(long long now_ms, long long next_status_ms,
                                  long long next_rescan_ms,
                                  long long next_repeat_ms, int device_count,
                                  int held_key)
{
    long long next = next_status_ms;
    long long remaining;

    if (!device_count && next_rescan_ms < next)
        next = next_rescan_ms;
    if (held_key && next_repeat_ms < next)
        next = next_repeat_ms;
    remaining = next - now_ms;
    if (remaining <= 0)
        return 0;
    return remaining > INT_MAX ? INT_MAX : remaining;
}

int buttond_repeat_due(long long now_ms, long long next_repeat_ms,
                       int held_key)
{
    return held_key && now_ms >= next_repeat_ms;
}
