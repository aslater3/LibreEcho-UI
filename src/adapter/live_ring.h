#ifndef LIBREECHO_LIVE_RING_H
#define LIBREECHO_LIVE_RING_H

/*
 * Sample-indexed preroll ring.
 *
 * The wake word fires after the user has already started speaking, so a Live
 * session that opens only at the wake event clips the first syllable of the
 * command.  lived therefore subscribes to waked's post-AEC stream continuously
 * while GPT-Live mode is selected and keeps the last few seconds on the device.
 * Nothing leaves the device until a wake event names a sample index.
 *
 * Indexing is absolute: every stored sample has a global sample number, frames
 * from waked carry `first_sample`, and a wake event carries
 * `detection_sample`.  That makes the "where do I start sending" question a
 * subtraction rather than a search, and it is the same indexing the existing
 * Wyoming path uses.
 *
 * Bounded: capacity is fixed at init and never grows.  A discontinuity in the
 * incoming sample numbering (a restarted capture, a dropped frame) resets the
 * ring rather than silently splicing unrelated audio together.
 */

#include <stddef.h>
#include <stdint.h>

#define LE_LIVE_RING_SECONDS 3U
#define LE_LIVE_RING_RATE 16000U
#define LE_LIVE_RING_CAPACITY (LE_LIVE_RING_RATE * LE_LIVE_RING_SECONDS)

struct le_live_ring {
    int16_t samples[LE_LIVE_RING_CAPACITY];
    /* Oldest retained sample number. */
    uint64_t first_sample;
    /* Number of valid samples, first_sample .. first_sample + count - 1. */
    size_t count;
    /* Set once anything has been stored; an empty ring has no position. */
    int primed;
};

void le_live_ring_reset(struct le_live_ring *ring);

/*
 * Append one sample-indexed frame.  `first_sample` must continue the current
 * numbering; a gap or a rewind resets the ring and starts again from this
 * frame.  Returns 0 on success, -1 on invalid arguments.
 */
int le_live_ring_append(struct le_live_ring *ring, uint64_t first_sample,
                        const int16_t *samples, size_t count);

/* Oldest sample number still retained, or 0 when nothing is retained. */
uint64_t le_live_ring_begin(const struct le_live_ring *ring);

/* One past the newest sample number, or 0 when nothing is retained. */
uint64_t le_live_ring_end(const struct le_live_ring *ring);

/*
 * Choose the first sample to send upstream for a wake detected at
 * `detection_sample`.
 *
 * The result is clamped to the retained window, so a wake that arrives with
 * less than `preroll_samples` of history still starts at the oldest sample we
 * actually have rather than at a fictitious index.  Returns 0 when the ring
 * holds nothing at or after the requested point.
 */
uint64_t le_live_ring_start_for_wake(const struct le_live_ring *ring,
                                     uint64_t detection_sample,
                                     uint64_t preroll_samples);

/*
 * Copy up to `count` samples starting at `first_sample`.  Returns the number of
 * samples written.  A range that starts before the retained window returns 0:
 * the requested audio is gone, and the caller is told so rather than handed
 * different audio at the same index.
 */
size_t le_live_ring_read(const struct le_live_ring *ring, uint64_t first_sample,
                         int16_t *out, size_t count);

#endif
