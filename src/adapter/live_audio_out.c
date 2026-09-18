#define _POSIX_C_SOURCE 200809L

#include "live_audio_out.h"
#include "radio_resample.h"
#include "playback_status_client.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_BUS "/run/libreecho-audio/system.pcm"
#define DEFAULT_SPEAKER_RATE 48000U
/* Longest a single back-pressure wait may hold off a cancellation check. */
#define WRITE_POLL_MS 40
/* Frames of resampler output the staging buffer can hold. */
#define STAGING_FRAMES (LE_LIVE_AUDIO_OUT_STAGING / 2U)

static int open_bus(struct le_live_audio_out *out)
{
    if (out->opened)
        return 0;
    /*
     * A FIFO with no reader fails the open with ENXIO rather than blocking
     * forever, which is what we want: audiod being down is a condition to
     * report and retry, not to hang the wake loop on.
     */
    out->fd = open(out->path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (out->fd < 0) {
        ++out->write_errors;
        return -1;
    }
    out->opened = 1;
    return 0;
}

void le_live_audio_out_init(struct le_live_audio_out *out, const char *bus_path,
                            unsigned int speaker_rate)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    out->speaker_rate = speaker_rate ? speaker_rate : DEFAULT_SPEAKER_RATE;
    snprintf(out->path, sizeof(out->path), "%s",
             bus_path && bus_path[0] ? bus_path : DEFAULT_BUS);
    {
        const char *slash = strrchr(out->path, '/');
        size_t directory = slash ? (size_t)(slash - out->path) : 0U;

        if (directory > 0 && directory + sizeof("/status.json") <=
            sizeof(out->status_path)) {
            memcpy(out->status_path, out->path, directory);
            memcpy(out->status_path + directory, "/status.json",
                   sizeof("/status.json"));
            memcpy(out->control_path, out->path, directory);
            memcpy(out->control_path + directory, "/control.sock",
                   sizeof("/control.sock"));
        }
    }
    le_radio_resample_reset(&out->resampler);
}

static int write_all(struct le_live_audio_out *out, const int16_t *samples,
                     size_t count)
{
    const unsigned char *position = (const unsigned char *)samples;
    size_t remaining = count * sizeof(*samples);
    unsigned int stalled_ms = 0;

    while (remaining) {
        struct pollfd descriptor;
        ssize_t written;

        if (out->cancelled)
            return -1;
        if (open_bus(out) < 0)
            return -1;
        written = write(out->fd, position, remaining);
        if (written < 0 && errno == EINTR)
            continue;
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /*
             * Playback back-pressure: the bus is draining in real time.  Wait
             * briefly and re-check cancellation rather than blocking on a
             * write that could outlive the turn.
             */
            ++out->stalls;
            stalled_ms += WRITE_POLL_MS;
            if (stalled_ms >= LE_LIVE_AUDIO_OUT_STALL_BUDGET_MS) {
                /*
                 * The bus is not draining.  Give up on this chunk rather than
                 * block the wake path; the session closes with a bounded error
                 * and re-arms for the next utterance.
                 */
                ++out->stall_timeouts;
                ++out->write_errors;
                return -1;
            }
            descriptor.fd = out->fd;
            descriptor.events = POLLOUT;
            descriptor.revents = 0;
            if (poll(&descriptor, 1, WRITE_POLL_MS) < 0 && errno != EINTR)
                return -1;
            continue;
        }
        if (written <= 0) {
            /* Reader went away (EPIPE) or the FIFO is unusable. */
            ++out->write_errors;
            close(out->fd);
            out->fd = -1;
            out->opened = 0;
            ++out->reopenings;
            return -1;
        }
        position += written;
        remaining -= (size_t)written;
    }
    return 0;
}

int le_live_audio_out_write(struct le_live_audio_out *out,
                            const int16_t *samples, size_t count,
                            unsigned int rate)
{
    size_t offset = 0;

    if (!out || !samples || !count || count > LE_LIVE_AUDIO_SAMPLES)
        return -1;
    /*
     * A write after a cancellation is the start of a new turn: the previous
     * turn's audio was dropped and playback may resume.
     */
    out->cancelled = 0;
    if (!rate)
        rate = LE_LIVE_INPUT_RATE;
    /*
     * The shared resampler targets the platform bus rate, which is what audiod
     * consumes.  A different configured rate would silently produce audio at
     * the wrong pitch, so it is refused rather than approximated.
     */
    if (out->speaker_rate != DEFAULT_SPEAKER_RATE) {
        ++out->write_errors;
        return -1;
    }
    if (rate < LE_LIVE_AUDIO_OUT_MIN_RATE) {
        ++out->write_errors;
        return -1;
    }
    /*
     * Continuity is per turn.  Carrying the resampler's phase across a
     * barge-in would splice the tail of a cancelled sentence into the start of
     * the next one.
     */
    if (!out->turn_open) {
        le_radio_resample_reset(&out->resampler);
        out->turn_open = 1;
    }

    while (offset < count) {
        size_t frames = count - offset;
        int produced;

        if (frames > LE_LIVE_AUDIO_OUT_SUBBLOCK_FRAMES)
            frames = LE_LIVE_AUDIO_OUT_SUBBLOCK_FRAMES;
        produced = le_radio_resample(&out->resampler,
                                     (const short *)(samples + offset),
                                     (int)frames, LE_LIVE_INPUT_CHANNELS,
                                     (int)rate, out->staging,
                                     (int)STAGING_FRAMES);
        if (produced < 0) {
            ++out->write_errors;
            return -1;
        }
        if (produced > 0) {
            if (write_all(out, out->staging,
                          (size_t)produced * 2U) < 0)
                return -1;
            out->frames_written += (uint64_t)produced;
        }
        offset += frames;
    }
    return 0;
}

int le_live_audio_out_drained(struct le_live_audio_out *out)
{
    int pending = 0;
    int drained;

    if (!out)
        return 0;
    ++out->drain_checks;
    /* FIONREAD on a FIFO writer reports bytes not yet consumed by the audio
       engine. A regular-file test sink may return ENOTTY and falls through to
       the engine status below. */
    if (out->fd >= 0 && ioctl(out->fd, FIONREAD, &pending) == 0 && pending > 0)
        return 0;
    drained = le_playback_status_bus_drained(out->status_path, "system");
    if (drained != 1)
        return 0;
    ++out->drain_confirmations;
    return 1;
}

void le_live_audio_out_cancel(struct le_live_audio_out *out)
{
    struct sockaddr_un address;
    static const char message[] = "cancel system";
    int fd;

    if (!out)
        return;
    ++out->cancel_requests;
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0 || !out->control_path[0] ||
        strlen(out->control_path) >= sizeof(address.sun_path)) {
        ++out->cancel_failures;
    } else {
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        strcpy(address.sun_path, out->control_path);
        if (sendto(fd, message, sizeof(message) - 1U, MSG_DONTWAIT | MSG_NOSIGNAL,
                   (struct sockaddr *)&address, sizeof(address)) !=
            (ssize_t)(sizeof(message) - 1U))
            ++out->cancel_failures;
    }
    if (fd >= 0)
        close(fd);
    out->cancelled = 1;
    /*
     * Close the turn so the next one starts from a clean resampler phase.
     */
    out->turn_open = 0;
    ++out->cancels;
    /*
     * The bus itself is a stream into audiod's own ring, so the strongest
     * guarantee available at this layer is that nothing further is written
     * and the writer returns immediately.  Anything audiod has already read
     * is bounded by one period plus the FIFO, not by the length of the
     * conversation.
     */
}

void le_live_audio_out_close(struct le_live_audio_out *out)
{
    if (!out)
        return;
    out->cancelled = 1;
    out->turn_open = 0;
    if (out->fd >= 0)
        close(out->fd);
    out->fd = -1;
    out->opened = 0;
}

void le_live_audio_out_metrics_json(const struct le_live_audio_out *out,
                                    char *json, size_t size)
{
    int written;

    if (!json || !size)
        return;
    json[0] = '\0';
    if (!out)
        return;
    written = snprintf(
        json, size,
        "{\"frames_written\":%llu,\"stalls\":%llu,\"stall_timeouts\":%llu,"
        "\"cancels\":%llu,\"reopenings\":%llu,\"write_errors\":%llu,"
        "\"drain_checks\":%llu,\"drain_confirmations\":%llu,"
        "\"cancel_requests\":%llu,\"cancel_failures\":%llu,"
        "\"bus\":\"%s\",\"speaker_rate\":%u}",
        (unsigned long long)out->frames_written,
        (unsigned long long)out->stalls,
        (unsigned long long)out->stall_timeouts,
        (unsigned long long)out->cancels,
        (unsigned long long)out->reopenings,
        (unsigned long long)out->write_errors,
        (unsigned long long)out->drain_checks,
        (unsigned long long)out->drain_confirmations,
        (unsigned long long)out->cancel_requests,
        (unsigned long long)out->cancel_failures,
        out->path, out->speaker_rate);
    if (written < 0 || (size_t)written >= size)
        json[size - 1] = '\0';
}