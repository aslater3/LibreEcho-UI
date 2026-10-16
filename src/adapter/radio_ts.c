#include "radio_ts.h"

#include <string.h>

#define PKT LE_RADIO_TS_PACKET
#define PLD LE_RADIO_TS_PAYLOAD

void le_radio_ts_reset(struct le_radio_ts *ts)
{
    memset(ts, 0, sizeof(*ts));
    ts->pmt_pid = -1;
    ts->audio_pid = -1;
}

/*
 * Locate a packet's payload. Returns 0 for a packet that carries none (a bare
 * adaptation field, or the reserved control value), 1 otherwise with *pay and
 * *len set. A truncated adaptation field is treated as payload-less rather
 * than trusted.
 */
static int payload_span(const unsigned char *pkt, const unsigned char **pay,
                        size_t *len)
{
    unsigned int control = (pkt[3] >> 4) & 3;

    if (control == 0 || control == 2)
        return 0;
    if (control == 3) {
        unsigned int af = pkt[4];

        if (5u + af > (unsigned int)PKT)
            return 0;
        *pay = pkt + 5 + af;
        *len = (size_t)(PKT - 5 - af);
    } else {
        *pay = pkt + 4;
        *len = PLD;
    }
    return *len > 0;
}

/* PAT: first program's PMT PID. Sections are single-packet here. */
static void parse_pat(struct le_radio_ts *ts, const unsigned char *pay,
                      size_t len)
{
    size_t ptr, section, end, i;

    if (len < 1)
        return;
    ptr = pay[0];
    if (1 + ptr + 12 > len)
        return;
    pay += 1 + ptr;
    len -= 1 + ptr;
    if (pay[0] != 0x00)                         /* table_id */
        return;
    section = ((size_t)(pay[1] & 0x0f) << 8) | pay[2];
    if (section < 9 || 3 + section > len)
        return;
    end = 3 + section - 4;                      /* stop before the CRC */
    for (i = 8; i + 4 <= end; i += 4) {
        if (((pay[i] << 8) | pay[i + 1]) != 0) {
            ts->pmt_pid = ((pay[i + 2] & 0x1f) << 8) | pay[i + 3];
            return;
        }
    }
}

/*
 * PMT: the first elementary stream that is ADTS AAC (0x0F) or MPEG audio
 * (0x03/0x04, passed through for the MP3 decoder). CRC is not checked.
 */
static void parse_pmt(struct le_radio_ts *ts, const unsigned char *pay,
                      size_t len)
{
    size_t ptr, section, info, end, i;

    if (len < 1)
        return;
    ptr = pay[0];
    if (1 + ptr + 13 > len)
        return;
    pay += 1 + ptr;
    len -= 1 + ptr;
    if (pay[0] != 0x02)                         /* table_id */
        return;
    section = ((size_t)(pay[1] & 0x0f) << 8) | pay[2];
    if (section < 13 || 3 + section > len)
        return;
    info = ((size_t)(pay[10] & 0x0f) << 8) | pay[11];
    end = 3 + section - 4;
    i = 12 + info;
    while (i + 5 <= end) {
        unsigned int type = pay[i];
        size_t es = ((size_t)(pay[i + 3] & 0x0f) << 8) | pay[i + 4];

        if (type == 0x0f || type == 0x03 || type == 0x04) {
            int pid = ((pay[i + 1] & 0x1f) << 8) | pay[i + 2];

            if (ts->audio_pid != pid)
                ts->in_pes = 0;                 /* the ES changed under us */
            ts->audio_pid = pid;
            return;
        }
        i += 5 + es;
    }
}

/*
 * Turn one packet's payload into ADTS bytes. Returns the count written at tmp
 * (never more than PLD) and updates the PAT/PMT/PES state.
 */
static size_t process(struct le_radio_ts *ts, const unsigned char *pkt,
                      unsigned char *tmp)
{
    const unsigned char *pay;
    size_t len;
    int pid;

    if (((pkt[3] >> 6) & 3) != 0)               /* scrambled: skip */
        return 0;
    if (!payload_span(pkt, &pay, &len))
        return 0;
    pid = ((pkt[1] & 0x1f) << 8) | pkt[2];

    if (pid == 0x0000) {
        if ((pkt[1] & 0x40) != 0)
            parse_pat(ts, pay, len);
        return 0;
    }
    if (pid == ts->pmt_pid) {
        if ((pkt[1] & 0x40) != 0)
            parse_pmt(ts, pay, len);
        return 0;
    }
    if (ts->audio_pid < 0 || pid != ts->audio_pid)
        return 0;                               /* every other PID is ignored */

    if ((pkt[1] & 0x40) != 0) {                 /* payload_unit_start: PES */
        size_t header;

        ts->in_pes = 0;                         /* old PES ends at a new start */
        if (len < 9 || pay[0] != 0x00 || pay[1] != 0x00 || pay[2] != 0x01) {
            ts->in_pes = 0;                     /* not a PES start: resync */
            return 0;
        }
        header = 9 + pay[8];                    /* 9 + PES_header_data_length */
        if (header > len)
            return 0;                           /* split header: wait for next start */
        ts->in_pes = 1;
        pay += header;
        len -= header;
    } else if (!ts->in_pes) {
        return 0;                               /* continuation with no start */
    }

    memcpy(tmp, pay, len);
    return len;
}

int le_radio_ts_feed(struct le_radio_ts *ts, const unsigned char *in, size_t n,
                     unsigned char *out, size_t out_cap, size_t *consumed)
{
    unsigned char tmp[PLD];
    size_t pos = 0, produced = 0;

    if (consumed)
        *consumed = 0;
    if (!ts || !in || !out || !consumed || out_cap < PLD)
        return -1;

    for (;;) {
        if (ts->synced) {
            if (ts->carry_used < PKT) {
                size_t take = PKT - ts->carry_used;

                if (take > n - pos)
                    take = n - pos;
                memcpy(ts->carry + ts->carry_used, in + pos, take);
                ts->carry_used += take;
                pos += take;
                if (ts->carry_used < PKT)
                    break;                      /* need more input */
            }
            if (ts->carry[0] == 0x47) {
                size_t add = process(ts, ts->carry, tmp);

                if (add > out_cap - produced)
                    break;                      /* out full: caller re-feeds */
                memcpy(out + produced, tmp, add);
                produced += add;
                memmove(ts->carry, ts->carry + PKT, ts->carry_used - PKT);
                ts->carry_used -= PKT;
                continue;
            }
            ts->synced = 0;                     /* bad sync: hunt in payload */
            ts->lost = 0;
        }

        /* Resyncing: need a whole packet plus the next packet's sync byte. */
        if (ts->carry_used < PKT + 1) {
            size_t take = PKT + 1 - ts->carry_used;

            if (take > n - pos)
                take = n - pos;
            memcpy(ts->carry + ts->carry_used, in + pos, take);
            ts->carry_used += take;
            pos += take;
            if (ts->carry_used < PKT + 1)
                break;                          /* need more input */
        }
        {
            size_t i, at = 0;                   /* at+1, 0 means not found */

            for (i = 0; i + PKT + 1 <= ts->carry_used; ++i)
                if (ts->carry[i] == 0x47 && ts->carry[i + PKT] == 0x47) {
                    at = i + 1;
                    break;
                }
            if (at == 0) {
                /* No candidate here: drop up to the last whole packet. */
                size_t drop = ts->carry_used > (size_t)PKT
                            ? ts->carry_used - PKT : 1;

                ts->lost += (int)drop;
                memmove(ts->carry, ts->carry + drop, ts->carry_used - drop);
                ts->carry_used -= drop;
                if (ts->lost > LE_RADIO_TS_RESYNC_PACKETS * PKT)
                    return -1;                  /* sync is gone for good */
                continue;
            }
            --at;                               /* byte offset of the lock */
            ts->lost += (int)at;
            memmove(ts->carry, ts->carry + at, ts->carry_used - at);
            ts->carry_used -= at;
            ts->synced = 1;
        }
    }

    *consumed = pos;
    return (int)produced;
}
