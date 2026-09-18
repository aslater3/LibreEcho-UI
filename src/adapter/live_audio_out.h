#ifndef LIBREECHO_LIVE_AUDIO_OUT_H
#define LIBREECHO_LIVE_AUDIO_OUT_H

/*
 * GPT-Live model speech into the central playback bus.
 *
 * lived must never open the codec or an ALSA device.  Model audio goes to the
 * same bus the Wyoming satellite and the local assistant already use, so
 * volume, mute, AirPlay arbitration, the AEC reference and every
 * hardware-specific codec detail stay inside audiod where they already live.
 *
 * The rate conversion is not reimplemented here: it is the repository's
 * existing shared resampler (`le_radio_resample`), which already carries phase
 * and history across blocks and is covered by its own tests.  A second
 * converter would be a second place for the channel mapping to be wrong.
 *
 * Two properties matter here and neither is free:
 *
 *   - Bounded buffering.  A model that talks faster than the bus drains must
 *     not grow a queue in lived's heap.  Writes are chunked and back-pressure
 *     is a short poll, not an allocation.
 *
 *   - Cancellation.  Barge-in and session close have to stop playback
 *     promptly.  A blocking write to the bus would make that impossible, so
 *     the bus is opened non-blocking and the write loop re-checks the cancel
 *     flag between chunks.
 */

#include "live_transport.h"
#include "radio_resample.h"

#include <stddef.h>
#include <stdint.h>

#define LE_LIVE_AUDIO_OUT_PATH_MAX 128
#define LE_LIVE_AUDIO_OUT_STATUS_PATH_MAX 160
/*
 * Longest a single chunk may spend waiting on playback back-pressure.  lived
 * runs one poll() loop; a bus that never drains must not be able to hold that
 * loop - and with it the wake path and every session timeout - forever.
 */
#define LE_LIVE_AUDIO_OUT_STALL_BUDGET_MS 2000U
/*
 * The shared resampler consumes at most one codec frame at a time, so a
 * transport chunk is split into sub-blocks of this size.  The resampler's
 * phase and history carry across the split, which is what it is built for.
 */
#define LE_LIVE_AUDIO_OUT_SUBBLOCK_FRAMES 1152U
/* Lowest source rate whose output still fits the staging buffer. */
#define LE_LIVE_AUDIO_OUT_MIN_RATE 8000U
/*
 * Staging for the resampler output: one sub-block at the lowest accepted rate
 * (8 kHz to 48 kHz is 6x) in interleaved stereo.
 */
#define LE_LIVE_AUDIO_OUT_STAGING \
    (LE_LIVE_AUDIO_OUT_SUBBLOCK_FRAMES * 6U * 2U)

struct le_live_audio_out {
    int fd;
    int opened;
    char path[LE_LIVE_AUDIO_OUT_PATH_MAX];
    char status_path[LE_LIVE_AUDIO_OUT_STATUS_PATH_MAX];
    char control_path[LE_LIVE_AUDIO_OUT_STATUS_PATH_MAX];
    unsigned int speaker_rate;
    int cancelled;
    /*
     * Resampler continuity is per turn.  Carrying phase across a barge-in
     * would splice the tail of a cancelled sentence into the start of the next
     * one.
     */
    int turn_open;
    struct le_radio_resampler resampler;
    int16_t staging[LE_LIVE_AUDIO_OUT_STAGING];
    uint64_t frames_written;
    uint64_t stalls;
    uint64_t stall_timeouts;
    uint64_t cancels;
    uint64_t reopenings;
    uint64_t write_errors;
    uint64_t drain_checks;
    uint64_t drain_confirmations;
    uint64_t cancel_requests;
    uint64_t cancel_failures;
};

/*
 * `bus_path` defaults to the system playback bus when NULL; `speaker_rate`
 * defaults to 48000 when zero.  Never fails: an unavailable bus is reported on
 * the first write and retried, because losing wake handling over a missing bus
 * would be worse than silence.
 */
void le_live_audio_out_init(struct le_live_audio_out *out, const char *bus_path,
                            unsigned int speaker_rate);

/*
 * Convert and play one chunk.  `samples` is mono/16-bit at `rate` under
 * `LE_LIVE_INPUT_CHANNELS`.  Returns 0 when the chunk reached the bus, -1 when
 * playback was cancelled, the bus refused the write, or the rate is outside
 * the range the fixed staging buffer can carry.
 */
int le_live_audio_out_write(struct le_live_audio_out *out,
                            const int16_t *samples, size_t count,
                            unsigned int rate);

/* True only after the FIFO and audio engine's system bus are both empty. */
int le_live_audio_out_drained(struct le_live_audio_out *out);

/*
 * Drop everything still queued and stop playing.  Called on barge-in, on
 * session close and on any failure path, so a truncated model turn can never
 * bleed into the next one.
 */
void le_live_audio_out_cancel(struct le_live_audio_out *out);

void le_live_audio_out_close(struct le_live_audio_out *out);

void le_live_audio_out_metrics_json(const struct le_live_audio_out *out,
                                    char *json, size_t size);

#endif
