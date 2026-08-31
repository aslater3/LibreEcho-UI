#include "wake_accept.h"

int le_wake_accept(float score, float accept_threshold, unsigned int support,
                   int vad_active, int playback_active)
{
    unsigned int required = playback_active
        ? LE_WAKE_SUPPORT_REQUIRED_PLAYING
        : LE_WAKE_SUPPORT_REQUIRED;

    if (!vad_active)
        return 0;
    if (score < accept_threshold)
        return 0;
    return support >= required;
}
