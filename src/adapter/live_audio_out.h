#ifndef LIBREECHO_LIVE_AUDIO_OUT_H
#define LIBREECHO_LIVE_AUDIO_OUT_H

/*
 * Hardware-independent streaming output. Each reply has an immutable managed
 * PCM connection; a separate focus lease spans the conversation. The shared
 * Platform render engine owns the codec, amplifier, mixing and AEC reference.
 *
 * Conversion uses the shared stateful resampler. Fixed queues preserve frames
 * across nonblocking EAGAIN without preventing wake/control handling. Ordered
 * FINISH flushes finite tails; only the engine's per-stream played cursor
 * confirms completion. Disconnect cancels just this reply's generation.
 */

#include "live_transport.h"
#include "radio_resample.h"
#include "pcm_stream_client.h"

#include <stddef.h>
#include <stdint.h>

#define LE_LIVE_AUDIO_OUT_PATH_MAX 128
#define LE_LIVE_AUDIO_OUT_STATUS_PATH_MAX 160
/*
 * Maximum elapsed time without queued-output progress. Each pump itself is
 * nonblocking; this deadline reports a failed sink rather than holding the loop.
 */
#define LE_LIVE_AUDIO_OUT_STALL_BUDGET_MS 2000U
#define LE_LIVE_AUDIO_OUT_QUEUE_FRAMES 16384U
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
    int focus_fd;
    int opened;
    int allow_legacy;
    int finishing;
    int finish_sent;
    size_t queued_frames;
    int16_t queue[LE_LIVE_AUDIO_OUT_QUEUE_FRAMES * 2U];
    uint64_t stalled_since_ms;
    uint64_t source_frames;
    uint64_t produced_frames;
    uint64_t turn_frames_written;
    struct le_pcm_progress progress;
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

/* The main loop services output independently of model/network events. */
int le_live_audio_out_pump(struct le_live_audio_out *out);
int le_live_audio_out_ready(const struct le_live_audio_out *out);
int le_live_audio_out_focus(struct le_live_audio_out *out, int enabled);
uint64_t le_live_audio_out_played_ms(const struct le_live_audio_out *out);
/* FINISH drains this stream, never an unrelated cue or another bus. */
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
