#define _POSIX_C_SOURCE 200809L
#include "live_audio_out.h"
#include "playback_status_client.h"
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#define DEFAULT_BUS "/run/libreecho-audio/system.pcm"
#define DEFAULT_SPEAKER_RATE 48000U
#define STAGING_FRAMES (LE_LIVE_AUDIO_OUT_STAGING / 2U)

static int open_bus(struct le_live_audio_out *out)
{
    if (out->opened) return 0;
    out->fd = le_pcm_open(out->path, 1U, 0, out->allow_legacy);
    if (out->fd < 0) { ++out->write_errors; return -1; }
    out->opened = 1; return 0;
}
void le_live_audio_out_init(struct le_live_audio_out *out, const char *path,
                            unsigned int speaker_rate)
{
    const char *legacy = getenv("LE_LIVE_ALLOW_LEGACY_TEST_SINK");
    const char *slash;
    size_t directory;
    if (!out) return;
    memset(out, 0, sizeof(*out)); out->fd = out->focus_fd = -1;
    out->speaker_rate = speaker_rate ? speaker_rate : DEFAULT_SPEAKER_RATE;
    out->allow_legacy = legacy && !strcmp(legacy, "1");
    snprintf(out->path, sizeof(out->path), "%s", path && path[0] ? path : DEFAULT_BUS);
    slash = strrchr(out->path, '/'); directory = slash ? (size_t)(slash - out->path) : 0;
    if (directory && directory + sizeof("/status.json") <= sizeof(out->status_path)) {
        memcpy(out->status_path, out->path, directory);
        memcpy(out->status_path + directory, "/status.json", sizeof("/status.json"));
    }
    le_radio_resample_reset(&out->resampler);
}
int le_live_audio_out_focus(struct le_live_audio_out *out, int enabled)
{
    if (!out) return -1;
    if (!enabled) {
        if (out->focus_fd >= 0) close(out->focus_fd);
        out->focus_fd = -1; return 0;
    }
    if (out->focus_fd >= 0 || out->allow_legacy) return 0;
    out->focus_fd = le_pcm_open(out->path, 1U, 1, 0);
    return out->focus_fd >= 0 ? 0 : -1;
}
int le_live_audio_out_ready(const struct le_live_audio_out *out)
{
    /* Reserve one maximal 8 kHz input event plus the resampler's final lookahead. */
    return out && !out->finishing && out->queued_frames +
        LE_LIVE_AUDIO_SAMPLES * 6U + 16U <= LE_LIVE_AUDIO_OUT_QUEUE_FRAMES;
}
static int queue_samples(struct le_live_audio_out *out, const int16_t *pcm, size_t frames)
{
    if (frames > LE_LIVE_AUDIO_OUT_QUEUE_FRAMES - out->queued_frames) return -1;
    memcpy(out->queue + out->queued_frames * 2U, pcm, frames * LE_PCM_FRAME_BYTES);
    out->queued_frames += frames; out->produced_frames += frames; return 0;
}
int le_live_audio_out_pump(struct le_live_audio_out *out)
{
    unsigned int budget;
    struct le_pcm_progress focus;
    if (!out) return -1;
    if (out->focus_fd >= 0 && le_pcm_progress_read(out->focus_fd, &focus) < 0)
        return -1;
    if (out->fd >= 0 && le_pcm_is_stream(out->fd) &&
        le_pcm_progress_read(out->fd, &out->progress) < 0) return -1;
    if (out->progress.state == LE_PCM_FAILED || out->progress.state == LE_PCM_CANCELLED)
        return -1;
    for (budget = 0; out->queued_frames && budget < 8U; ++budget) {
        ssize_t written;
        if (out->cancelled || open_bus(out) < 0) return -1;
        written = le_pcm_write(out->fd, out->queue, out->queued_frames * LE_PCM_FRAME_BYTES);
        if (written < 0 && errno == EINTR) continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            uint64_t now = le_pcm_monotonic_ms();
            ++out->stalls;
            if (!out->stalled_since_ms) out->stalled_since_ms = now;
            if (now - out->stalled_since_ms >= LE_LIVE_AUDIO_OUT_STALL_BUDGET_MS) {
                ++out->stall_timeouts; ++out->write_errors; return -1;
            }
            return 0; /* Retain the bytes and return to wake/control handling. */
        }
        if (written <= 0 || (size_t)written % LE_PCM_FRAME_BYTES) {
            ++out->write_errors; return -1;
        }
        out->stalled_since_ms = 0;
        out->queued_frames -= (size_t)written / LE_PCM_FRAME_BYTES;
        out->frames_written += (uint64_t)written / LE_PCM_FRAME_BYTES;
        out->turn_frames_written += (uint64_t)written / LE_PCM_FRAME_BYTES;
        memmove(out->queue, (unsigned char *)out->queue + written,
                out->queued_frames * LE_PCM_FRAME_BYTES);
    }
    if (out->finishing && !out->queued_frames && !out->finish_sent) {
        if (le_pcm_finish(out->fd) == 0) out->finish_sent = 1;
        else if (errno != EAGAIN && errno != EWOULDBLOCK) return -1;
    }
    return 0;
}
int le_live_audio_out_write(struct le_live_audio_out *out, const int16_t *pcm,
                            size_t count, unsigned int rate)
{
    size_t offset = 0;
    if (!out || !pcm || !count || count > LE_LIVE_AUDIO_SAMPLES) return -1;
    if (!rate) rate = LE_LIVE_INPUT_RATE;
    if (out->speaker_rate != DEFAULT_SPEAKER_RATE || rate < LE_LIVE_AUDIO_OUT_MIN_RATE ||
        rate > 192000U || !le_live_audio_out_ready(out)) {
        ++out->write_errors; return -1;
    }
    if (!out->turn_open) {
        if (out->fd >= 0) close(out->fd);
        out->fd = -1; out->opened = 0; out->cancelled = 0;
        out->source_frames = out->produced_frames = out->turn_frames_written = 0;
        memset(&out->progress, 0, sizeof(out->progress));
        le_radio_resample_reset(&out->resampler); out->turn_open = 1;
    } else if (out->resampler.rate && out->resampler.rate != (int)rate) {
        ++out->write_errors; return -1; /* Format changes require a new stream. */
    }
    if (open_bus(out) < 0) return -1;
    while (offset < count) {
        size_t frames = count - offset;
        int produced;
        if (frames > LE_LIVE_AUDIO_OUT_SUBBLOCK_FRAMES) frames = LE_LIVE_AUDIO_OUT_SUBBLOCK_FRAMES;
        produced = le_radio_resample(&out->resampler, pcm + offset, (int)frames,
                                      1, (int)rate, out->staging, (int)STAGING_FRAMES);
        if (produced < 0 || queue_samples(out, out->staging, (size_t)produced) < 0) return -1;
        offset += frames; out->source_frames += frames;
    }
    return le_live_audio_out_pump(out);
}
static int finish_turn(struct le_live_audio_out *out)
{
    if (!out->finishing && out->turn_open) {
        uint64_t target = out->source_frames * DEFAULT_SPEAKER_RATE /
                          (unsigned int)out->resampler.rate;
        short tail[4];
        int produced;
        tail[0] = tail[2] = out->resampler.history[(LE_RADIO_RESAMPLE_HISTORY - 1U) * 2U];
        tail[1] = tail[3] = out->resampler.history[(LE_RADIO_RESAMPLE_HISTORY - 1U) * 2U + 1U];
        /* Supply lookahead, then trim to the finite stream's exact duration. */
        produced = le_radio_resample(&out->resampler, tail, 2, 2, out->resampler.rate,
                                      out->staging, (int)STAGING_FRAMES);
        if (produced < 0 || out->produced_frames > target) return -1;
        if ((uint64_t)produced > target - out->produced_frames)
            produced = (int)(target - out->produced_frames);
        if (queue_samples(out, out->staging, (size_t)produced) < 0 ||
            out->produced_frames != target) return -1;
        out->finishing = 1;
    }
    return le_live_audio_out_pump(out);
}
int le_live_audio_out_drained(struct le_live_audio_out *out)
{
    int pending = 0, drained;
    if (!out) return 0;
    ++out->drain_checks;
    if (!out->turn_open) return 1;
    if (finish_turn(out) < 0) return 0;
    if (out->queued_frames || !out->finish_sent) return 0;
    if (le_pcm_is_stream(out->fd)) {
        drained = out->progress.state == LE_PCM_DRAINED &&
                  out->progress.accepted_frames == out->turn_frames_written &&
                  out->progress.played_frames == out->turn_frames_written;
    } else {
        /* Explicit host-test/legacy mode only; no stream-scoped guarantees. */
        if (ioctl(out->fd, FIONREAD, &pending) == 0 && pending > 0) return 0;
        drained = le_playback_status_bus_drained(out->status_path, "system") == 1;
    }
    if (!drained) return 0;
    ++out->drain_confirmations;
    out->turn_open = out->finishing = out->finish_sent = 0;
    return 1;
}
uint64_t le_live_audio_out_played_ms(const struct le_live_audio_out *out)
{
    return out ? out->progress.played_frames * 1000U / DEFAULT_SPEAKER_RATE : 0;
}
void le_live_audio_out_cancel(struct le_live_audio_out *out)
{
    if (!out) return;
    ++out->cancel_requests; ++out->cancels;
    /* Disconnect is an unambiguous cancellation of this connection's immutable
     * generation, including DATA still in transit. It cannot clear another
     * stream, a cue, or media. Already mixed hardware audio is bounded by ALSA. */
    if (out->fd >= 0) close(out->fd);
    out->fd = -1; out->opened = 0; out->cancelled = 1;
    out->turn_open = out->finishing = out->finish_sent = 0;
    out->queued_frames = 0; out->stalled_since_ms = 0;
    memset(&out->progress, 0, sizeof(out->progress));
}
void le_live_audio_out_close(struct le_live_audio_out *out)
{
    if (!out) return;
    le_live_audio_out_cancel(out); (void)le_live_audio_out_focus(out, 0);
}
void le_live_audio_out_metrics_json(const struct le_live_audio_out *out, char *json, size_t size)
{
    int n;
    if (!out || !json || !size) return;
    n = snprintf(json, size,
        "{\"frames_written\":%llu,\"stalls\":%llu,\"stall_timeouts\":%llu,"
        "\"cancels\":%llu,\"reopenings\":%llu,\"write_errors\":%llu,"
        "\"drain_checks\":%llu,\"drain_confirmations\":%llu,"
        "\"cancel_requests\":%llu,\"cancel_failures\":%llu,"
        "\"queued_frames\":%zu,\"played_frames\":%llu,\"managed\":%s,"
        "\"bus\":\"%s\",\"speaker_rate\":%u}",
        (unsigned long long)out->frames_written, (unsigned long long)out->stalls,
        (unsigned long long)out->stall_timeouts, (unsigned long long)out->cancels,
        (unsigned long long)out->reopenings, (unsigned long long)out->write_errors,
        (unsigned long long)out->drain_checks, (unsigned long long)out->drain_confirmations,
        (unsigned long long)out->cancel_requests, (unsigned long long)out->cancel_failures,
        out->queued_frames, (unsigned long long)out->progress.played_frames,
        out->allow_legacy ? "false" : "true", out->path, out->speaker_rate);
    if (n < 0 || (size_t)n >= size)
        snprintf(json, size, "{\"error\":\"audio metrics exceed budget\"}");
}
