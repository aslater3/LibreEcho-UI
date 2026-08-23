#include "adapter/buttond_timing.h"

#include <assert.h>
#include <limits.h>

int main(void)
{
    /* An open, idle device must still wake for the status heartbeat. */
    assert(buttond_poll_timeout_ms(1000, 16000, 6000, 0, 1, 0) == 15000);
    /* Hold-repeat remains the earlier wake-up when a key is held. */
    assert(buttond_poll_timeout_ms(1000, 16000, 6000, 1200, 1, 1) == 200);
    /* With no devices, discovery can be earlier than the heartbeat. */
    assert(buttond_poll_timeout_ms(1000, 16000, 3000, 0, 0, 0) == 2000);
    assert(buttond_poll_timeout_ms(16000, 16000, 20000, 0, 1, 0) == 0);
    assert(buttond_poll_timeout_ms(0, (long long)INT_MAX + 100, 0, 0, 1, 0) == INT_MAX);
    assert(buttond_repeat_due(1200, 1200, 1));
    assert(!buttond_repeat_due(1199, 1200, 1));
    assert(!buttond_repeat_due(2000, 1200, 0));
    return 0;
}
