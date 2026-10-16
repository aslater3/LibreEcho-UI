/*
 * Ogg Opus decode for radiod (see radio_opus.h for the contract).
 *
 * The seekable local file is presented to libopusfile through read/seek/tell
 * callbacks over the caller's fd; radiod owns the fd and the bus, so the close
 * callback is a no-op.  libopusfile validates OpusHead at open (a non-Opus Ogg
 * or a truncated file is refused there), reports the true channel count (a mono
 * link is upmixed to stereo by op_read_stereo), and follows chained streams
 * link by link; the decode loop is therefore a bounded pull-to-EOF with no
 * unbounded buffering of its own.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include "radio_opus.h"

#ifdef LE_RADIOD_ENABLE_OPUS

#include <opusfile.h>

/* 120 ms of 48 kHz stereo per pull: comfortably above the 20 ms Opus frame. */
#define LE_RADIO_OPUS_MAX_FRAMES 5760
#define LE_RADIO_OPUS_CHANNELS 2

struct le_radio_opus_source {
    int fd;
};

static int opus_read_cb(void *src, unsigned char *ptr, int n)
{
    struct le_radio_opus_source *s = src;
    ssize_t r;

    do {
        r = read(s->fd, ptr, (size_t)n);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
        return OP_EREAD;
    return (int)r;
}

static int opus_seek_cb(void *src, opus_int64 offset, int whence)
{
    struct le_radio_opus_source *s = src;

    return lseek(s->fd, (off_t)offset, whence) < 0 ? -1 : 0;
}

static opus_int64 opus_tell_cb(void *src)
{
    struct le_radio_opus_source *s = src;
    off_t position = lseek(s->fd, 0, SEEK_CUR);

    return position < 0 ? 0 : (opus_int64)position;
}

/* radiod owns the fd; closing it is not the decoder's job. */
static int opus_close_cb(void *src)
{
    (void)src;
    return 0;
}

/*
 * Fold a libopusfile return code into the small, stable set radio_opus.h
 * promises.  A container that is not Opus, a bad header, a bad link and a
 * timestamp violation are all "this is not a decodable Opus stream"; genuine
 * read failures are "I/O".
 */
static int opus_error_code(int error)
{
    switch (error) {
    case OP_EREAD:
    case OP_EFAULT:
    case OP_EIMPL:
    case OP_EINVAL:
        return LE_RADIO_OPUS_IO;
    default:
        return LE_RADIO_OPUS_NOT_OPUS;
    }
}

static int write_bus_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = write(fd, bytes + sent, length - sent);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        sent += (size_t)n;
    }
    return 0;
}

int le_radio_opus_available(void)
{
    return 1;
}

int le_radio_opus_play_fd(int bus_fd, int file_fd, long *played, int *complete)
{
    static const OpusFileCallbacks callbacks = {
        opus_read_cb, opus_seek_cb, opus_tell_cb, opus_close_cb
    };
    struct le_radio_opus_source source;
    OggOpusFile *file;
    opus_int16 pcm[LE_RADIO_OPUS_MAX_FRAMES * LE_RADIO_OPUS_CHANNELS];
    long total = 0;
    int error = 0, rc = LE_RADIO_OPUS_OK;

    /*
     * A caller reusing the out-parameters must read an honest "nothing
     * decoded, not complete" unless a clean decode overwrites it: a malformed
     * or truncated file never claims frames or completion.
     */
    if (played)
        *played = 0;
    if (complete)
        *complete = 0;
    if (bus_fd < 0 || file_fd < 0)
        return LE_RADIO_OPUS_IO;
    if (lseek(file_fd, 0, SEEK_SET) < 0)
        return LE_RADIO_OPUS_IO;
    source.fd = file_fd;
    file = op_open_callbacks(&source, &callbacks, NULL, 0, &error);
    if (!file)
        return opus_error_code(error);
    for (;;) {
        int got = op_read_stereo(file, pcm, LE_RADIO_OPUS_MAX_FRAMES);

        if (got == 0) {                  /* clean end of the last link */
            if (complete)
                *complete = 1;
            break;
        }
        if (got < 0) {                   /* bounded: no partial success claim */
            rc = opus_error_code(got);
            break;
        }
        if (write_bus_all(bus_fd, pcm,
                          (size_t)got * LE_RADIO_OPUS_CHANNELS *
                          sizeof(opus_int16)) < 0) {
            rc = LE_RADIO_OPUS_BUS;
            break;
        }
        total += got;
    }
    op_free(file);
    if (played)
        *played = total;
    return rc;
}

#else /* !LE_RADIOD_ENABLE_OPUS */

/*
 * Capability-disabled build: say so, do not pretend.  radiod refuses an Ogg
 * file with a clear reason instead of handing it to minimp3 and reporting a
 * silent station.
 */
int le_radio_opus_available(void)
{
    return 0;
}

int le_radio_opus_play_fd(int bus_fd, int file_fd, long *played, int *complete)
{
    (void)bus_fd;
    (void)file_fd;
    (void)played;
    (void)complete;
    return LE_RADIO_OPUS_UNSUPPORTED;
}

#endif /* LE_RADIOD_ENABLE_OPUS */
