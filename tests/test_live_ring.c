#define _POSIX_C_SOURCE 200809L

/* Preroll ring indexing and wake-sample selection. */

#include "adapter/live_ring.h"

#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed at %s:%d: %s\n", \
                __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

static struct le_live_ring ring;

static int fill(uint64_t first_sample, size_t count)
{
    int16_t buffer[LE_LIVE_RING_CAPACITY];
    size_t i;

    for (i = 0; i < count; ++i)
        buffer[i] = (int16_t)(first_sample + i);
    return le_live_ring_append(&ring, first_sample, buffer, count);
}

static int test_empty_ring(void)
{
    le_live_ring_reset(&ring);
    CHECK(le_live_ring_begin(&ring) == 0);
    CHECK(le_live_ring_end(&ring) == 0);
    CHECK(le_live_ring_start_for_wake(&ring, 100, 10) == 0);
    CHECK(le_live_ring_read(&ring, 0, (int16_t *)ring.samples, 4) == 0);
    return 0;
}

static int test_indexing_round_trip(void)
{
    int16_t out[8];

    le_live_ring_reset(&ring);
    CHECK(fill(1000, 1000) == 0);
    CHECK(le_live_ring_begin(&ring) == 1000);
    CHECK(le_live_ring_end(&ring) == 2000);
    CHECK(le_live_ring_read(&ring, 1500, out, 4) == 4);
    CHECK(out[0] == 1500);
    CHECK(out[3] == 1503);
    /* A read that runs past the newest sample is clamped, not zero-filled. */
    CHECK(le_live_ring_read(&ring, 1998, out, 8) == 2);
    /* A read entirely before the window yields nothing. */
    CHECK(le_live_ring_read(&ring, 10, out, 4) == 0);
    return 0;
}

static int test_wraparound_drops_oldest(void)
{
    int16_t out[4];

    le_live_ring_reset(&ring);
    CHECK(fill(0, LE_LIVE_RING_CAPACITY) == 0);
    CHECK(le_live_ring_begin(&ring) == 0);
    /* One more sample evicts exactly one from the front. */
    CHECK(fill(LE_LIVE_RING_CAPACITY, 1) == 0);
    CHECK(le_live_ring_begin(&ring) == 1);
    CHECK(le_live_ring_end(&ring) == LE_LIVE_RING_CAPACITY + 1);
    CHECK(le_live_ring_read(&ring, 1, out, 4) == 4);
    CHECK(out[0] == 1);
    return 0;
}

static int test_discontinuity_resets(void)
{
    int16_t out[4];

    le_live_ring_reset(&ring);
    CHECK(fill(0, 100) == 0);
    /* A frame that does not continue the numbering must not be spliced in. */
    CHECK(fill(5000, 10) == 0);
    CHECK(le_live_ring_begin(&ring) == 5000);
    CHECK(le_live_ring_end(&ring) == 5010);
    CHECK(le_live_ring_read(&ring, 5000, out, 4) == 4);
    CHECK(out[0] == 5000);
    return 0;
}

static int test_wake_selection_clamps_to_window(void)
{
    le_live_ring_reset(&ring);
    CHECK(fill(16000, 16000) == 0);

    /* Ordinary case: back off by the configured preroll. */
    CHECK(le_live_ring_start_for_wake(&ring, 20000, 2400) == 17600);
    /* Preroll larger than the history available starts at the oldest sample
       rather than at an index we no longer hold. */
    CHECK(le_live_ring_start_for_wake(&ring, 16000, 4800) == 16000);
    /* A wake before anything we retained still starts inside the window. */
    CHECK(le_live_ring_start_for_wake(&ring, 100, 2400) == 16000);
    /* A wake at the newest sample backs off into audio we still hold. */
    CHECK(le_live_ring_start_for_wake(&ring, 32000, 2400) == 29600);
    /* A wake past the window entirely has nothing to send yet. */
    CHECK(le_live_ring_start_for_wake(&ring, 40000, 2400) == 0);
    return 0;
}

static int test_partial_frame_sequence(void)
{
    int16_t out[8];

    le_live_ring_reset(&ring);
    /* Three small frames, exactly how waked delivers 10 ms of 16 kHz audio. */
    CHECK(fill(0, 160) == 0);
    CHECK(fill(160, 160) == 0);
    CHECK(fill(320, 160) == 0);
    CHECK(le_live_ring_end(&ring) == 480);
    CHECK(le_live_ring_read(&ring, 160, out, 4) == 4);
    CHECK(out[0] == 160);
    return 0;
}

int main(void)
{
    if (test_empty_ring() || test_indexing_round_trip() ||
        test_wraparound_drops_oldest() || test_discontinuity_resets() ||
        test_wake_selection_clamps_to_window() ||
        test_partial_frame_sequence())
        return 1;
    printf("live ring: ok\n");
    return 0;
}
