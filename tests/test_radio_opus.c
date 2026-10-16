/*
 * Ogg Opus decode: real libopusfile behaviour, both build flavours.
 *
 * This test is compiled twice by tests/run_radiod_opus_tests.sh:
 *
 *   - with -DLE_RADIOD_ENABLE_OPUS and the pinned OPUS_PREFIX archives, it
 *     decodes the generated fixtures and checks the promise radio_opus.h
 *     makes to radiod (frame counts, mono upmix, chained links, honest
 *     errors, bounded I/O, no silent success, 16-bit little-endian bus
 *     bytes, bus-gone cancellation);
 *   - without the macro, it checks the honest stub: the capability reads 0
 *     and playback is refused with LE_RADIO_OPUS_UNSUPPORTED.
 *
 * The fixtures are embedded bytes (radiod_opus_fixture.h), so the enabled run
 * needs only the pinned libraries, never ffmpeg or the network.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>

#include "../src/adapter/radio_opus.h"
#include "radiod_opus_fixture.h"

#ifdef LE_RADIOD_ENABLE_OPUS
#include <opusfile.h>
#endif

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        return 1; \
    } \
} while (0)

#define STEREO_FRAMES 4800
#define MONO_FRAMES 4800
#define CHAINED_FRAMES 9600

#ifdef LE_RADIOD_ENABLE_OPUS
static int host_is_little_endian(void)
{
    const unsigned short one = 1;

    return *(const unsigned char *)&one == 1;
}

/* Materialise a fixture as a seekable file and return a read-only fd. */
static int fixture_fd(const char *tag, const unsigned char *data,
                      unsigned long len, char *path_out, size_t path_size)
{
    int fd;

    snprintf(path_out, path_size, "/tmp/le-opus-%s-%ld.ogg", tag, (long)getpid());
    fd = open(path_out, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    if (len && write(fd, data, (size_t)len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    close(fd);
    return open(path_out, O_RDONLY | O_CLOEXEC);
}

static long sink_size(int fd)
{
    off_t end = lseek(fd, 0, SEEK_END);

    return end < 0 ? -1 : (long)end;
}

/*
 * Independent reference decode of a fixture to native int16 stereo.  The
 * decode wrapper under test must produce exactly these samples, serialised as
 * 16-bit little-endian, which is what pins the byte order.
 */
struct ref_pcm {
    int fd;
};

static int ref_read(void *src, unsigned char *p, int n)
{
    struct ref_pcm *r = src;
    ssize_t got;

    do {
        got = read(r->fd, p, (size_t)n);
    } while (got < 0 && errno == EINTR);
    return got < 0 ? OP_EREAD : (int)got;
}

static int ref_seek(void *src, opus_int64 off, int whence)
{
    struct ref_pcm *r = src;

    return lseek(r->fd, (off_t)off, whence) < 0 ? -1 : 0;
}

static opus_int64 ref_tell(void *src)
{
    struct ref_pcm *r = src;
    off_t pos = lseek(r->fd, 0, SEEK_CUR);

    return pos < 0 ? 0 : (opus_int64)pos;
}

static int ref_close(void *src)
{
    (void)src;
    return 0;
}
#endif

static int test_disabled_capability(void)
{
#ifndef LE_RADIOD_ENABLE_OPUS
    long played = -123;
    int complete = -123;

    CHECK(le_radio_opus_available() == 0);
    CHECK(le_radio_opus_play_fd(1, 0, &played, &complete) ==
          LE_RADIO_OPUS_UNSUPPORTED);
    /* The disabled build must not touch the out-parameters: it makes no
       claim about a stream it cannot read. */
    CHECK(played == -123 && complete == -123);
    CHECK(le_radio_opus_is_ogg((const unsigned char *)"OggS", 4) == 1);
    CHECK(le_radio_opus_is_ogg((const unsigned char *)"ID3", 3) == 0);
    puts("radio opus disabled capability: honest stub: ok");
    return 0;
#else
    return 0;
#endif
}

static int test_sniff(void)
{
    CHECK(le_radio_opus_is_ogg(le_opus_stereo, le_opus_stereo_len) == 1);
    CHECK(le_radio_opus_is_ogg(le_opus_mono, le_opus_mono_len) == 1);
    CHECK(le_radio_opus_is_ogg(le_opus_chained, le_opus_chained_len) == 1);
    CHECK(le_radio_opus_is_ogg(le_opus_truncated, le_opus_truncated_len) == 1);
    CHECK(le_radio_opus_is_ogg(le_opus_badhead, le_opus_badhead_len) == 1);
    CHECK(le_radio_opus_is_ogg(le_opus_junk, le_opus_junk_len) == 0);
    CHECK(le_radio_opus_is_ogg((const unsigned char *)"Ogg", 3) == 0);
    return 0;
}

#ifdef LE_RADIOD_ENABLE_OPUS
static int first_nonzero(const unsigned char *p, long bytes)
{
    long i;

    for (i = 0; i < bytes; ++i)
        if (p[i])
            return 1;
    return 0;
}

static int decode_case(const char *tag, const unsigned char *data,
                       unsigned long len, long expect_frames,
                       int expect_complete)
{
    char fpath[128], bpath[128];
    unsigned char chunk[65536];
    unsigned char bus_bytes[65536];
    struct ref_pcm ref;
    static const OpusFileCallbacks ref_cb = {
        ref_read, ref_seek, ref_tell, ref_close
    };
    long played = -1;
    int complete = -1, rc, file_fd, bus_fd;
    OggOpusFile *of = NULL;
    int ref_error = 0;
    long ref_frames = 0;
    long bytes;

    file_fd = fixture_fd(tag, data, len, fpath, sizeof(fpath));
    CHECK(file_fd >= 0);
    snprintf(bpath, sizeof(bpath), "/tmp/le-opus-%s-%ld.pcm", tag, (long)getpid());
    bus_fd = open(bpath, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(bus_fd >= 0);

    rc = le_radio_opus_play_fd(bus_fd, file_fd, &played, &complete);
    CHECK(rc == LE_RADIO_OPUS_OK);
    CHECK(complete == expect_complete);
    CHECK(played == expect_frames);
    bytes = sink_size(bus_fd);
    CHECK(bytes == expect_frames * 2 * 2);

    /* The bus must carry real audio, not zero-fill or silence. */
    CHECK(bytes <= (long)sizeof(bus_bytes));
    CHECK(lseek(bus_fd, 0, SEEK_SET) == 0);
    CHECK(read(bus_fd, bus_bytes, sizeof(bus_bytes)) == (ssize_t)bytes);
    CHECK(first_nonzero(bus_bytes, bytes) == 1);

    /*
     * Byte order and sample identity: an independent decode of the same
     * fixture must equal the bytes on the bus when both are read as 16-bit
     * little-endian.  On a little-endian host the bus is a raw copy of the
     * decoder's native samples, which is exactly the ARM32 media-bus format.
     */
    if (lseek(file_fd, 0, SEEK_SET) < 0)
        return 1;
    ref.fd = file_fd;
    of = op_open_callbacks(&ref, &ref_cb, NULL, 0, &ref_error);
    if (!of)
        return 1;
    for (;;) {
        int got = op_read_stereo(of, (opus_int16 *)chunk, 65536 / 4);

        if (got == 0)
            break;
        if (got < 0)
            break;
        ref_frames += got;
    }
    op_free(of);
    CHECK(ref_frames == expect_frames);
    if (host_is_little_endian()) {
        short native[2048 * 2];       /* frames * channels */
        int got;

        /* Re-open and compare the first block of native samples against the
           little-endian interpretation of the bus bytes. */
        if (lseek(file_fd, 0, SEEK_SET) < 0)
            return 1;
        ref.fd = file_fd;
        of = op_open_callbacks(&ref, &ref_cb, NULL, 0, &ref_error);
        CHECK(of != NULL);
        got = op_read_stereo(of, (opus_int16 *)native, 2048);
        CHECK(got > 0);
        {
            long i;
            const unsigned char *bus = bus_bytes;

            for (i = 0; i < (long)got * 2; ++i) {
                int as_le = (int)(short)(bus[i * 2] | (bus[i * 2 + 1] << 8));

                CHECK(as_le == (int)native[i]);
            }
        }
        op_free(of);
    }

    /*
     * Bounded I/O and repeatability: rewinding the same descriptor and
     * decoding again yields the same byte count, so the decoder neither
     * consumes past EOF nor silently short-changes a later run.
     */
    {
        long played2 = -1;
        int complete2 = -1;

        rc = le_radio_opus_play_fd(bus_fd, file_fd, &played2, &complete2);
        CHECK(rc == LE_RADIO_OPUS_OK);
        CHECK(played2 == expect_frames && complete2 == 1);
        CHECK(sink_size(bus_fd) == bytes * 2);
    }

    close(bus_fd);
    close(file_fd);
    unlink(fpath);
    unlink(bpath);
    return 0;
}

static int test_mono_upmix(void)
{
    char fpath[128], bpath[128];
    unsigned char *bus;
    long i;
    int file_fd, bus_fd;

    file_fd = fixture_fd("mono-upmix", le_opus_mono, le_opus_mono_len,
                         fpath, sizeof(fpath));
    CHECK(file_fd >= 0);
    snprintf(bpath, sizeof(bpath), "/tmp/le-opus-mono-upmix-%ld.pcm",
             (long)getpid());
    bus_fd = open(bpath, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(bus_fd >= 0);
    {
        long played = -1;
        int complete = -1;
        int rc = le_radio_opus_play_fd(bus_fd, file_fd, &played, &complete);

        CHECK(rc == LE_RADIO_OPUS_OK && played == MONO_FRAMES && complete == 1);
    }
    bus = malloc((size_t)MONO_FRAMES * 4);
    CHECK(bus != NULL);
    CHECK(lseek(bus_fd, 0, SEEK_SET) == 0);
    CHECK(read(bus_fd, bus, (size_t)MONO_FRAMES * 4) == MONO_FRAMES * 4);
    for (i = 0; i < MONO_FRAMES; ++i) {
        short left = (short)(bus[i * 4] | (bus[i * 4 + 1] << 8));
        short right = (short)(bus[i * 4 + 2] | (bus[i * 4 + 3] << 8));

        CHECK(left == right);              /* mono upmixed to both channels */
    }
    free(bus);
    close(bus_fd);
    close(file_fd);
    unlink(fpath);
    unlink(bpath);
    return 0;
}

static int test_reject(const char *tag, const unsigned char *data,
                       unsigned long len, int expect_ok)
{
    char fpath[128], bpath[128];
    long played = -1;
    int complete = 7;                     /* deliberately pre-set, must clear */
    int file_fd, bus_fd, rc;

    file_fd = fixture_fd(tag, data, len, fpath, sizeof(fpath));
    CHECK(file_fd >= 0);
    snprintf(bpath, sizeof(bpath), "/tmp/le-opus-%s-%ld.pcm", tag, (long)getpid());
    bus_fd = open(bpath, O_RDWR | O_CREAT | O_TRUNC, 0600);
    CHECK(bus_fd >= 0);

    rc = le_radio_opus_play_fd(bus_fd, file_fd, &played, &complete);
    if (expect_ok) {
        CHECK(rc == LE_RADIO_OPUS_OK);
    } else {
        /* A malformed or truncated stream must be refused, never reported as
           a completed decode, and must leave no partial frames behind. */
        CHECK(rc != LE_RADIO_OPUS_OK);
        CHECK(complete == 0);
        CHECK(played == 0);
        CHECK(sink_size(bus_fd) == 0);
    }
    close(bus_fd);
    close(file_fd);
    unlink(fpath);
    unlink(bpath);
    return 0;
}

static int test_cancellation_and_bad_fds(void)
{
    char fpath[128];
    int file_fd, pipefd[2];
    long played = -1;
    int complete = 7;
    int rc;

    /* A bus that has gone away: the reader end is closed, so the first write
       fails and the decode must stop with LE_RADIO_OPUS_BUS, not spin or
       claim success. */
    file_fd = fixture_fd("cancel", le_opus_chained, le_opus_chained_len,
                         fpath, sizeof(fpath));
    CHECK(file_fd >= 0);
    CHECK(pipe(pipefd) == 0);
    close(pipefd[0]);
    rc = le_radio_opus_play_fd(pipefd[1], file_fd, &played, &complete);
    CHECK(rc == LE_RADIO_OPUS_BUS);
    CHECK(complete == 0);
    close(pipefd[1]);

    /* Invalid descriptors are refused before any I/O. */
    CHECK(le_radio_opus_play_fd(-1, file_fd, &played, &complete) ==
          LE_RADIO_OPUS_IO);
    CHECK(le_radio_opus_play_fd(1, -1, &played, &complete) == LE_RADIO_OPUS_IO);

    close(file_fd);
    unlink(fpath);
    return 0;
}
#endif /* LE_RADIOD_ENABLE_OPUS */

int main(void)
{
    /* The bus is a socket/pipe owned by another process; a reader that goes
       away must surface as a bounded error, exactly as the daemons arrange. */
    (void)signal(SIGPIPE, SIG_IGN);
    CHECK(test_sniff() == 0);
    CHECK(test_disabled_capability() == 0);
#ifdef LE_RADIOD_ENABLE_OPUS
    /* Enabled build only: the real decoder must be present. */
    CHECK(le_radio_opus_available() == 1);

    CHECK(decode_case("stereo", le_opus_stereo, le_opus_stereo_len,
                      STEREO_FRAMES, 1) == 0);
    CHECK(decode_case("mono", le_opus_mono, le_opus_mono_len,
                      MONO_FRAMES, 1) == 0);
    CHECK(decode_case("chained", le_opus_chained, le_opus_chained_len,
                      CHAINED_FRAMES, 1) == 0);

    CHECK(test_mono_upmix() == 0);
    CHECK(test_reject("truncated", le_opus_truncated, le_opus_truncated_len,
                      0) == 0);
    CHECK(test_reject("badhead", le_opus_badhead, le_opus_badhead_len, 0) == 0);
    CHECK(test_reject("junk", le_opus_junk, le_opus_junk_len, 0) == 0);
    CHECK(test_cancellation_and_bad_fds() == 0);
    puts("radio opus decode: mono/stereo/chained frames, upmix, reject, "
         "byte order, bounded I/O, cancellation: ok");
#else
    puts("radio opus disabled capability: stub refusal: ok");
#endif
    return 0;
}
