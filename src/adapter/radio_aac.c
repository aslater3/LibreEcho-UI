#include "radio_aac.h"

#include <stdint.h>
#include <string.h>

#include "aacdec.h"

/*
 * Static decoder state. AACInitDecoderPre carves the decoder's structures out
 * of this buffer; its interior offsets are rounded up to 8, and the SBR state
 * pointer it hands back is used without further alignment, so the backing
 * store is an array of uint64_t (8-byte aligned) rather than unsigned char.
 * No malloc on the audio path: the only allocation is this fixed static.
 *
 * 81920 B >= AACDecInfo (120) + PSInfoBase (28752) + PSInfoSBR (50788),
 * rounded to 8, with slack.
 */
#define LE_RADIO_AAC_STATE_BYTES 81920

/* Complete ADTS header without CRC. frame_length is bits 12..0 of bytes 3..5. */
#define LE_RADIO_AAC_HEADER_BYTES 7

/* Bounded: give up after this many consecutive rejected frames. */
#define LE_RADIO_AAC_MAX_REJECTS 50

static uint64_t le_radio_aac_state[LE_RADIO_AAC_STATE_BYTES / sizeof(uint64_t)];
static HAACDecoder le_radio_aac_handle;
static int le_radio_aac_rejects;

/* 13-bit adts_frame_length from the fixed header at p[0..5]. */
static size_t aac_frame_length(const unsigned char *p)
{
    return ((size_t)(p[3] & 3) << 11) | ((size_t)p[4] << 3) | ((size_t)p[5] >> 5);
}

int le_radio_aac_open(void)
{
    le_radio_aac_rejects = 0;
    le_radio_aac_handle = AACInitDecoderPre(le_radio_aac_state,
                                            (int)sizeof(le_radio_aac_state));
    return le_radio_aac_handle ? 0 : -1;
}

void le_radio_aac_close(void)
{
    /* State stays in the static buffer; only the handle is dropped. */
    le_radio_aac_handle = 0;
    le_radio_aac_rejects = 0;
}

int le_radio_aac_take_frame(unsigned char *in, size_t *filled, size_t capacity,
                            short *pcm, int *channels, int *rate)
{
    AACFrameInfo info;
    unsigned char *ptr;
    size_t frame_len;
    int sync, bytes_left, err;

    if (!le_radio_aac_handle || !in || !filled || !pcm)
        return -2;

    for (;;) {
        if (*filled < LE_RADIO_AAC_HEADER_BYTES)
            return *filled >= capacity ? -2 : -1;

        /* Resync: drop everything before the next ADTS sync word. */
        sync = AACFindSyncWord(in, (int)*filled);
        if (sync < 0) {
            /* No sync in the buffer: drop the junk but keep the last byte in
               case a sync straddles the next read. */
            if (*filled >= capacity)
                return -2;
            in[0] = in[*filled - 1];
            *filled = 1;
            return -1;
        }
        if (sync > 0) {
            memmove(in, in + sync, *filled - (size_t)sync);
            *filled -= (size_t)sync;
        }

        if (*filled < LE_RADIO_AAC_HEADER_BYTES)
            return *filled >= capacity ? -2 : -1;

        frame_len = aac_frame_length(in);
        if (frame_len < LE_RADIO_AAC_HEADER_BYTES) {
            /* Header claims a sub-header frame: malformed. Drop the sync word
               and resync from the next byte. */
            memmove(in, in + 1, *filled - 1);
            *filled -= 1;
            if (++le_radio_aac_rejects >= LE_RADIO_AAC_MAX_REJECTS) {
                le_radio_aac_rejects = 0;
                return -2;
            }
            continue;
        }

        if (*filled < frame_len) {
            /* A whole frame is not buffered yet; retain the partial tail. */
            return *filled >= capacity ? -2 : -1;
        }

        /* A complete ADTS frame is at the head: decode it, then consume it. */
        ptr = in;
        bytes_left = (int)frame_len;
        err = AACDecode(le_radio_aac_handle, &ptr, &bytes_left, pcm);
        memmove(in, in + frame_len, *filled - frame_len);
        *filled -= frame_len;

        if (err)
            goto reject;

        AACGetLastFrameInfo(le_radio_aac_handle, &info);
        if (info.nChans <= 0 || info.nChans > 2 || info.outputSamps <= 0 ||
            info.outputSamps > LE_RADIO_AAC_MAX_SAMPLES)
            goto reject;                 /* >2 channels or a bad frame shape */

        le_radio_aac_rejects = 0;
        if (channels)
            *channels = info.nChans;
        if (rate)
            *rate = info.sampRateOut;
        return info.outputSamps;

reject:
        /* A rejected complete frame was dropped above, not the session. */
        if (++le_radio_aac_rejects >= LE_RADIO_AAC_MAX_REJECTS) {
            le_radio_aac_rejects = 0;
            return -2;
        }
        return 0;
    }
}

int le_radio_aac_is_adts(const unsigned char *p, size_t n)
{
    if (!p || n < 2)
        return 0;
    /* 0xFFF sync (top 12 bits) and layer bits (bits 2..1 of p[1]) == 0. */
    return p[0] == 0xFF && (p[1] & 0xF6) == 0xF0;
}
