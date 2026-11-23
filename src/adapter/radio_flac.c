/*
 * FLAC decode for radiod. See radio_flac.h.
 */
#include "radio_flac.h"

#include <string.h>

#include "dr_flac.h"

struct source {
    le_radio_flac_read_fn read;
    void *ctx;
    long long position;
    int failed;                       /* read error, as opposed to the end */
};

/* dr_flac wants the whole request unless the stream has ended. */
static size_t on_read(void *user, void *out, size_t want)
{
    struct source *src = user;
    size_t got = 0;

    while (got < want && !src->failed) {
        long n = src->read(src->ctx, (unsigned char *)out + got, want - got);

        if (n < 0)
            src->failed = 1;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    src->position += (long long)got;
    return got;
}

/*
 * Forward-only. Skipping metadata (artwork, padding) is a forward relative
 * seek and is honoured by reading and discarding; seeking back to the seek
 * table, or to the end to learn the size, is refused and dr_flac carries on
 * without them.
 */
static drflac_bool32 on_seek(void *user, int offset, drflac_seek_origin origin)
{
    struct source *src = user;
    long long skip;
    unsigned char scratch[1024];

    if (offset < 0)
        return DRFLAC_FALSE;
    if (origin == DRFLAC_SEEK_CUR)
        skip = offset;
    else if (origin == DRFLAC_SEEK_SET && offset >= src->position)
        skip = offset - src->position;
    else
        return DRFLAC_FALSE;
    while (skip > 0) {
        size_t chunk = skip > (long long)sizeof(scratch) ? sizeof(scratch)
                                                         : (size_t)skip;

        if (on_read(src, scratch, chunk) != chunk)
            return DRFLAC_FALSE;
        skip -= (long long)chunk;
    }
    return DRFLAC_TRUE;
}

int le_radio_flac_is_flac(const unsigned char *data, size_t length)
{
    return length >= 4 && !memcmp(data, "fLaC", 4);
}

int le_radio_flac_play(le_radio_flac_read_fn read, void *read_ctx,
                       le_radio_flac_sink_fn sink, void *sink_ctx,
                       long long *frames_out)
{
    static short pcm[LE_RADIO_FLAC_BLOCK * 2];
    struct source src;
    drflac *flac;
    int rc = LE_RADIO_FLAC_OK;
    long long total = 0;

    if (frames_out)
        *frames_out = 0;
    memset(&src, 0, sizeof(src));
    src.read = read;
    src.ctx = read_ctx;
    flac = drflac_open(on_read, on_seek, NULL, &src, NULL);
    if (!flac)
        return src.failed ? LE_RADIO_FLAC_SOURCE : LE_RADIO_FLAC_NOT_FLAC;
    if (flac->channels < 1 || flac->channels > 2 || !flac->sampleRate) {
        drflac_close(flac);
        return LE_RADIO_FLAC_UNSUPPORTED;
    }
    for (;;) {
        drflac_uint64 got = drflac_read_pcm_frames_s16(
            flac, LE_RADIO_FLAC_BLOCK, (drflac_int16 *)pcm);

        if (!got)
            break;
        if (sink(sink_ctx, pcm, (int)got, (int)flac->channels,
                 (int)flac->sampleRate) < 0) {
            rc = LE_RADIO_FLAC_SINK;
            break;
        }
        total += (long long)got;
    }
    /*
     * A stream that stopped short is a source failure, so the caller can
     * reconnect and resume. STREAMINFO's total, when declared, tells a clean
     * end from a truncated one even when the source returned 0 rather than
     * an error.
     */
    if (rc == LE_RADIO_FLAC_OK &&
        (src.failed || (flac->totalPCMFrameCount &&
                        (drflac_uint64)total < flac->totalPCMFrameCount)))
        rc = LE_RADIO_FLAC_SOURCE;
    drflac_close(flac);
    if (frames_out)
        *frames_out = total;
    return rc;
}
