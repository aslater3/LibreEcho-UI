#ifndef LE_WAKE_DIAGNOSTIC_H
#define LE_WAKE_DIAGNOSTIC_H
#include "backend.h"
#include "adapter/wake_health.h"
/* Shared API/diagnostic rule. Unknown or stale observations never mean healthy. */
static inline const char *le_wake_diagnostic(const struct le_wake_word_state *w,
                                            int result, int muted, int development)
{
    if (muted) return "muted";
    if (result != LE_OK) return "degraded";
    if (!w->enabled) return "disabled";
    if (!w->health_available || !w->model_loaded || !w->processed_frames ||
        !w->capture_active || !w->inference_active ||
        !le_wake_recent(w->capture_age_ms, LE_WAKE_CAPTURE_STALE_MS) ||
        !le_wake_recent(w->inference_age_ms, LE_WAKE_INFERENCE_STALE_MS))
        return "degraded";
    return development ? "development" : "ok";
}
#endif
