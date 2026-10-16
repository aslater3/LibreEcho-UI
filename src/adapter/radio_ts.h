/*
 * MPEG-TS to ADTS elementary-stream demultiplexing for the internet radio
 * player.
 *
 * Split out of radiod.c for the same reason radio_resample.c is: the packet
 * arithmetic is worth exercising on its own, and radiod.c pulls in minimp3 and
 * the socket layer, which a strict test build will not tolerate. It is a pure
 * push parser — no I/O, no allocation, fixed state — so the HLS fetcher can
 * stream a segment through it in whatever chunks the socket returns, including
 * a split in the middle of a packet or between segments.
 */
#ifndef LIBREECHO_RADIO_TS_H
#define LIBREECHO_RADIO_TS_H

#include <stddef.h>

#define LE_RADIO_TS_PACKET 188
#define LE_RADIO_TS_PAYLOAD 184                 /* 188 minus the 4-byte header */
#define LE_RADIO_TS_RESYNC_PACKETS 3

struct le_radio_ts {
    int pmt_pid;                                /* -1 until the PAT names it */
    int audio_pid;                              /* -1 until the PMT names it */
    int in_pes;                                 /* audio PID is mid-PES */
    int synced;                                 /* on the 188-byte grid */
    int lost;                                   /* bytes dropped since resync */
    /*
     * One packet plus one byte of the next: the extra byte is what lets the
     * resync scan demand 0x47 at +188 before trusting a lone sync byte that
     * happens to sit in payload.
     */
    unsigned char carry[LE_RADIO_TS_PACKET + 1];
    size_t carry_used;
};

void le_radio_ts_reset(struct le_radio_ts *ts);

/*
 * Push n bytes of a TS segment (any split, including mid-packet). Appends the
 * ADTS elementary-stream bytes to out, never writing more than out_cap, and
 * returns the number written. *consumed receives the input bytes taken; it is
 * below n only when out filled up, in which case the caller re-feeds from
 * in + *consumed. Returns -1 on a hard error: sync lost and not recovered
 * within 3 packets, or out_cap too small to hold one packet's payload (< 184).
 *
 * The state persists across calls and across segments: a new segment starts
 * with PAT/PMT again, and re-reading them is harmless.
 */
int le_radio_ts_feed(struct le_radio_ts *ts, const unsigned char *in, size_t n,
                     unsigned char *out, size_t out_cap, size_t *consumed);

#endif
