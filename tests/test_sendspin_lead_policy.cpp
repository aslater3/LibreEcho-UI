// LeadPolicy: short lead while the engine output is warm, the configured cold
// lead (0) otherwise. Pure logic; no SDK.
#include "lead_policy.h"

#include <cstdio>

using libreecho::sendspin::LeadPolicy;

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", msg); ++g_failures; } } while (0)

int main() {
    // Engine hold 5 s, margin 1 s: warm for 4 s after the last streaming poll.
    LeadPolicy policy(150, 5000000, 1000000);
    CHECK(policy.update(false, 0) == 0, "cold before any stream");
    CHECK(policy.update(true, 1000000) == 150, "warm while streaming");
    CHECK(policy.update(true, 9000000) == 150, "still warm while streaming");
    CHECK(policy.update(false, 9500000) == 150, "warm just after the stream ends (pause/seek)");
    CHECK(policy.update(false, 12999999) == 150, "warm until hold minus margin");
    CHECK(policy.update(false, 13000000) == 0, "cold once the engine may have closed the output");
    CHECK(policy.update(true, 20000000) == 150, "warm again on the next stream");
    LeadPolicy no_hold(150, 500000, 1000000);
    CHECK(no_hold.update(true, 0) == 150, "warm while streaming without a usable hold");
    CHECK(no_hold.update(false, 1) == 0, "a hold shorter than the margin is never trusted");
    if (g_failures) return 1;
    std::fprintf(stderr, "test_sendspin_lead_policy: all checks passed\n");
    return 0;
}
