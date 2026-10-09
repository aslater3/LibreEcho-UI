#define _POSIX_C_SOURCE 200809L

#include "live_ring.h"

#include <string.h>

void le_live_ring_reset(struct le_live_ring *ring)
{
    if (!ring)
        return;
    ring->first_sample = 0;
    ring->count = 0;
    ring->primed = 0;
}

int le_live_ring_append(struct le_live_ring *ring, uint64_t first_sample,
                        const int16_t *samples, size_t count)
{
    size_t i;

    if (!ring || !samples || count == 0 || count > LE_LIVE_RING_CAPACITY)
        return -1;
    /*
     * Continuity check.  A frame that does not continue the numbering is a
     * restarted or discontinuous capture; mixing it with what we already hold
     * would splice unrelated audio into the command.  Start over instead.
     */
    if (ring->primed && first_sample != ring->first_sample + ring->count)
        le_live_ring_reset(ring);
    if (!ring->primed) {
        ring->first_sample = first_sample;
        ring->count = 0;
        ring->primed = 1;
    }
    for (i = 0; i < count; ++i) {
        if (ring->count == LE_LIVE_RING_CAPACITY) {
            ++ring->first_sample;
            --ring->count;
        }
        ring->samples[(ring->first_sample + ring->count) %
                      LE_LIVE_RING_CAPACITY] = samples[i];
        ++ring->count;
    }
    return 0;
}

uint64_t le_live_ring_begin(const struct le_live_ring *ring)
{
    return ring && ring->count ? ring->first_sample : 0;
}

uint64_t le_live_ring_end(const struct le_live_ring *ring)
{
    return ring && ring->count ? ring->first_sample + ring->count : 0;
}

uint64_t le_live_ring_start_for_wake(const struct le_live_ring *ring,
                                     uint64_t detection_sample,
                                     uint64_t preroll_samples)
{
    uint64_t begin;
    uint64_t end;
    uint64_t start;

    if (!ring || !ring->count)
        return 0;
    begin = ring->first_sample;
    end = ring->first_sample + ring->count;
    start = detection_sample > preroll_samples
        ? detection_sample - preroll_samples : 0;
    if (start < begin)
        start = begin;
    return start >= end ? 0 : start;
}

size_t le_live_ring_read(const struct le_live_ring *ring, uint64_t first_sample,
                         int16_t *out, size_t count)
{
    uint64_t begin;
    uint64_t end;
    size_t available;
    size_t i;

    if (!ring || !out || count == 0 || !ring->count)
        return 0;
    begin = ring->first_sample;
    end = ring->first_sample + ring->count;
    /*
     * A read that starts before the retained window returns nothing rather
     * than silently substituting the oldest samples we happen to hold: the
     * caller asked for specific audio and is entitled to be told it is gone.
     * `start_for_wake` is where a wake point gets clamped into the window.
     */
    if (first_sample < begin)
        return 0;
    if (first_sample >= end)
        return 0;
    available = (size_t)(end - first_sample);
    if (count > available)
        count = available;
    for (i = 0; i < count; ++i)
        out[i] = ring->samples[(first_sample + i) % LE_LIVE_RING_CAPACITY];
    return count;
}
