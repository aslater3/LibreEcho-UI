/*
 * MPEG-TS -> ADTS demuxer tests, all against the committed r2 fixture:
 * aac-tone.ts must demux to aac-tone.adts byte-for-byte however the input is
 * split, a corrupted sync byte must resync, and non-audio PIDs must vanish.
 */
#include "adapter/radio_ts.h"

#include <stdio.h>
#include <string.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
        return 1; \
    } \
} while (0)

#define TS_PATH "tests/fixtures/radio/aac-tone.ts"
#define ADTS_PATH "tests/fixtures/radio/aac-tone.adts"
#define FILE_MAX 65536

/* Packet 54 of aac-tone.ts is a mid-stream audio continuation: 184 bytes of
   pure payload, no PES header, so dropping it removes exactly 184 ADTS bytes. */
#define BAD_AUDIO_PACKET 54
#define BAD_AUDIO_BYTES 184
/* Packet 19 is a repeated PAT: it carries no audio, so a corrupted sync byte
   there must be resynced past with the ADTS output unchanged. */
#define BAD_PAT_PACKET 19

static unsigned char ts[FILE_MAX], adts[FILE_MAX], out[FILE_MAX];
static size_t ts_len, adts_len, out_len;
static struct le_radio_ts st;

static size_t read_file(const char *path, unsigned char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n;

    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 0;
    }
    n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

/* Push a buffer through the live demuxer in chunks, appending to out. */
static int push(const unsigned char *data, size_t len, size_t chunk)
{
    size_t pos = 0;

    while (pos < len) {
        size_t take = len - pos < chunk ? len - pos : chunk;
        size_t consumed;
        int r = le_radio_ts_feed(&st, data + pos, take, out + out_len,
                                 FILE_MAX - out_len, &consumed);

        if (r < 0)
            return -1;
        out_len += (size_t)r;
        if (consumed == 0)
            return -1;                          /* out full: not expected */
        pos += consumed;
    }
    return 0;
}

/* Fresh state, one segment, given chunk size. */
static int demux_one(size_t chunk)
{
    le_radio_ts_reset(&st);
    out_len = 0;
    return push(ts, ts_len, chunk);
}

int main(void)
{
    static const size_t chunks[] = { 1, 7, 187, 189, 4096 };
    size_t i, off;

    ts_len = read_file(TS_PATH, ts, sizeof(ts));
    adts_len = read_file(ADTS_PATH, adts, sizeof(adts));
    CHECK(ts_len > 0);
    CHECK(adts_len > 0);
    CHECK(ts_len % LE_RADIO_TS_PACKET == 0);

    /* The whole segment in one push, then every split down to one byte. */
    CHECK(demux_one(ts_len) == 0);
    CHECK(out_len == adts_len);
    CHECK(memcmp(out, adts, adts_len) == 0);

    for (i = 0; i < sizeof(chunks) / sizeof(chunks[0]); ++i) {
        CHECK(demux_one(chunks[i]) == 0);
        CHECK(out_len == adts_len);
        CHECK(memcmp(out, adts, adts_len) == 0);
    }

    /* State persists across segments: a second segment with its own PAT/PMT
       must not disturb the output, which becomes the fixture twice over. */
    le_radio_ts_reset(&st);
    out_len = 0;
    CHECK(push(ts, ts_len, 500) == 0);
    CHECK(push(ts, ts_len, 500) == 0);
    CHECK(out_len == 2 * adts_len);
    CHECK(memcmp(out, adts, adts_len) == 0);
    CHECK(memcmp(out + adts_len, adts, adts_len) == 0);

    /* A corrupted PAT sync mid-stream resyncs, and the non-audio packet
       contributes nothing, so the ADTS is unchanged. */
    {
        unsigned char saved = ts[BAD_PAT_PACKET * LE_RADIO_TS_PACKET];

        ts[BAD_PAT_PACKET * LE_RADIO_TS_PACKET] = 0x11;
        CHECK(demux_one(300) == 0);
        CHECK(out_len == adts_len);
        CHECK(memcmp(out, adts, adts_len) == 0);
        ts[BAD_PAT_PACKET * LE_RADIO_TS_PACKET] = saved;
    }

    /* A corrupted audio sync resyncs on the next packet: the output is the
       fixture with that one packet's payload removed, so it matches up to the
       first divergence and again from BAD_AUDIO_BYTES later. */
    {
        unsigned char saved = ts[BAD_AUDIO_PACKET * LE_RADIO_TS_PACKET];

        ts[BAD_AUDIO_PACKET * LE_RADIO_TS_PACKET] = 0x11;
        CHECK(demux_one(500) == 0);
        ts[BAD_AUDIO_PACKET * LE_RADIO_TS_PACKET] = saved;
        CHECK(out_len == adts_len - BAD_AUDIO_BYTES);
        off = 0;
        while (off < out_len && out[off] == adts[off])
            ++off;
        CHECK(off > 0);
        CHECK(memcmp(out + off, adts + off + BAD_AUDIO_BYTES,
                     out_len - off) == 0);
    }

    /* out_cap below one packet's payload is a hard error. */
    {
        size_t consumed = 123;

        le_radio_ts_reset(&st);
        CHECK(le_radio_ts_feed(&st, ts, ts_len, out,
                               LE_RADIO_TS_PAYLOAD - 1, &consumed) == -1);
    }

    /* A new PES start whose header overruns the packet must end the previous
       PES: the continuation after it is dropped, not emitted as audio. */
    {
        unsigned char pk[2 * LE_RADIO_TS_PACKET];
        size_t before;

        le_radio_ts_reset(&st);
        out_len = 0;
        CHECK(push(ts, ts_len, 4096) == 0);      /* learns PAT/PMT, in a PES */
        before = out_len;
        memset(pk, 0xaa, sizeof(pk));
        pk[0] = 0x47; pk[1] = 0x41; pk[2] = 0x00; pk[3] = 0x10;  /* PUSI, pid 256 */
        pk[4] = 0x00; pk[5] = 0x00; pk[6] = 0x01; pk[7] = 0xc0;
        pk[12] = 255;                                /* header length > packet */
        pk[LE_RADIO_TS_PACKET] = 0x47; pk[LE_RADIO_TS_PACKET + 1] = 0x01;
        pk[LE_RADIO_TS_PACKET + 2] = 0x00; pk[LE_RADIO_TS_PACKET + 3] = 0x11;
        CHECK(push(pk, sizeof(pk), sizeof(pk)) == 0);
        CHECK(out_len == before);
    }

    printf("test_radio_ts OK\n");
    return 0;
}
