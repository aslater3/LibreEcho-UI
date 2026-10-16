#ifndef LE_WAKE_HEALTH_H
#define LE_WAKE_HEALTH_H
#include <limits.h>
#include <stdint.h>
#define LE_WAKE_CAPTURE_STALE_MS 2000
#define LE_WAKE_INFERENCE_STALE_MS 5000
/* Monotonic ages: -1 means no observation (including clock failure/reset).
 * A VAD-negative frame is still flowing capture; never use VAD as liveness. */
static inline int le_wake_age_ms(uint64_t now_ns, uint64_t last_ns)
{
    uint64_t age;
    if (!now_ns || !last_ns || now_ns < last_ns) return -1;
    age = (now_ns - last_ns) / 1000000ULL;
    return age > INT_MAX ? INT_MAX : (int)age;
}
static inline int le_wake_recent(int age_ms, int limit_ms)
{
    return age_ms >= 0 && age_ms <= limit_ms;
}
#endif
