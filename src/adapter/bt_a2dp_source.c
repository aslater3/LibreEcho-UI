/*
 * LibreEcho A2DP source-role protocol core (issue #411, first slice).
 * See bt_a2dp_source.h for scope and the bit layouts used here.
 */
#define _POSIX_C_SOURCE 200809L

#include "adapter/bt_a2dp_source.h"

#include <string.h>

#define AVDTP_PKT_SINGLE 0
#define AVDTP_MSG_COMMAND 0
#define AVDTP_MSG_ACCEPT 2
#define AVDTP_MSG_REJECT 3
#define AVDTP_MSG_GENERAL_REJECT 1

#define AVDTP_CAT_MEDIA_TRANSPORT 0x01
#define AVDTP_CAT_MEDIA_CODEC 0x07
#define AVDTP_MEDIA_TYPE_AUDIO 0
#define AVDTP_CODEC_SBC 0x00
#define AVDTP_SEP_TSEP_SINK 0x08 /* TSEP bit set: the endpoint is a sink */

#define SBC_FREQ_48K 0x10
#define SBC_FREQ_44K 0x20
#define SBC_CHAN_JOINT 0x01
#define SBC_CHAN_STEREO 0x02
#define SBC_CHAN_DUAL 0x04
#define SBC_CHAN_MONO 0x08
#define SBC_BLOCKS_16 0x10
#define SBC_BLOCKS_12 0x20
#define SBC_BLOCKS_8 0x40
#define SBC_BLOCKS_4 0x80
#define SBC_SUBBANDS_8 0x04
#define SBC_SUBBANDS_4 0x08
#define SBC_ALLOC_LOUDNESS 0x01
#define SBC_ALLOC_SNR 0x02
#define SBC_MIN_BITPOOL 2

#define RTP_HEADER_LENGTH 12
#define MEDIA_HEADER_LENGTH 1
#define MEDIA_HEADER_START_LAST 0x60 /* fragmented=0, start=1, last=1 */

/* Return the first preference bit present in mask, or 0. */
static uint8_t pick_bit(uint8_t mask, const uint8_t *preferred, size_t count)
{
    size_t i;

    for (i = 0; i < count; ++i) {
        if (mask & preferred[i])
            return preferred[i];
    }
    return 0;
}

size_t le_a2dp_src_build_command(uint8_t transaction, uint8_t signal_id,
                                 const uint8_t *payload, size_t payload_length,
                                 uint8_t *out, size_t out_max)
{
    if (!out || (payload_length && !payload))
        return 0;
    if (out_max < 2 || payload_length > out_max - 2)
        return 0;
    out[0] = (uint8_t)(((transaction & 0x0f) << 4) |
                       (AVDTP_PKT_SINGLE << 2) | AVDTP_MSG_COMMAND);
    out[1] = (uint8_t)(signal_id & 0x3f);
    if (payload_length)
        memcpy(out + 2, payload, payload_length);
    return 2 + payload_length;
}

int le_a2dp_src_response_payload(const uint8_t *packet, size_t length,
                                 uint8_t signal_id, const uint8_t **payload,
                                 size_t *payload_length)
{
    uint8_t message;

    if (!packet || !payload || !payload_length || length < 2)
        return -1;
    if (((packet[0] >> 2) & 0x03) != AVDTP_PKT_SINGLE)
        return -1;
    if ((packet[1] & 0x3f) != (signal_id & 0x3f))
        return -1;
    message = packet[0] & 0x03;
    if (message != AVDTP_MSG_ACCEPT)
        return -1;
    *payload = packet + 2;
    *payload_length = length - 2;
    return 0;
}

int le_a2dp_src_find_sink_seid(const uint8_t *payload, size_t length,
                               uint8_t *seid)
{
    size_t offset;

    if (!payload || !seid)
        return -1;
    for (offset = 0; offset + 2 <= length; offset += 2) {
        uint8_t sep_seid = (uint8_t)(payload[offset] >> 2);
        int in_use = (payload[offset] >> 1) & 0x01;
        uint8_t media_type = (uint8_t)(payload[offset + 1] >> 4);
        int tsep_sink = (payload[offset + 1] & AVDTP_SEP_TSEP_SINK) != 0;

        if (!in_use && media_type == AVDTP_MEDIA_TYPE_AUDIO && tsep_sink) {
            *seid = sep_seid;
            return 0;
        }
    }
    return -1;
}

int le_a2dp_src_parse_sbc_caps(const uint8_t *payload, size_t length,
                               struct le_a2dp_src_sbc_caps *caps)
{
    size_t offset = 0;

    if (!payload || !caps)
        return -1;
    while (offset + 2 <= length) {
        uint8_t category = payload[offset];
        uint8_t cap_length = payload[offset + 1];
        const uint8_t *value = payload + offset + 2;

        if (cap_length > length - offset - 2)
            return -1;
        if (category == AVDTP_CAT_MEDIA_CODEC && cap_length >= 6 &&
            (value[0] >> 4) == AVDTP_MEDIA_TYPE_AUDIO &&
            value[1] == AVDTP_CODEC_SBC) {
            caps->freq_channel = value[2];
            caps->blocks_subbands_alloc = value[3];
            caps->min_bitpool = value[4];
            caps->max_bitpool = value[5];
            return 0;
        }
        offset += 2 + cap_length;
    }
    return -1;
}

int le_a2dp_src_choose_sbc(const struct le_a2dp_src_sbc_caps *caps,
                           struct le_a2dp_src_sbc_config *config)
{
    static const uint8_t freq_pref[] = { SBC_FREQ_48K, SBC_FREQ_44K };
    static const uint8_t chan_pref[] = { SBC_CHAN_JOINT, SBC_CHAN_STEREO,
                                         SBC_CHAN_DUAL, SBC_CHAN_MONO };
    static const uint8_t blocks_pref[] = { SBC_BLOCKS_16, SBC_BLOCKS_12,
                                           SBC_BLOCKS_8, SBC_BLOCKS_4 };
    static const uint8_t subband_pref[] = { SBC_SUBBANDS_8, SBC_SUBBANDS_4 };
    static const uint8_t alloc_pref[] = { SBC_ALLOC_LOUDNESS, SBC_ALLOC_SNR };
    uint8_t freq, chan, blocks, subbands, alloc;
    uint8_t min_pool, max_pool;

    if (!caps || !config)
        return -1;
    freq = pick_bit(caps->freq_channel & 0xf0, freq_pref,
                    sizeof(freq_pref) / sizeof(freq_pref[0]));
    chan = pick_bit(caps->freq_channel & 0x0f, chan_pref,
                    sizeof(chan_pref) / sizeof(chan_pref[0]));
    blocks = pick_bit(caps->blocks_subbands_alloc & 0xf0, blocks_pref,
                      sizeof(blocks_pref) / sizeof(blocks_pref[0]));
    subbands = pick_bit(caps->blocks_subbands_alloc & 0x0c, subband_pref,
                        sizeof(subband_pref) / sizeof(subband_pref[0]));
    alloc = pick_bit(caps->blocks_subbands_alloc & 0x03, alloc_pref,
                     sizeof(alloc_pref) / sizeof(alloc_pref[0]));
    if (!freq || !chan || !blocks || !subbands || !alloc)
        return -1;

    min_pool = caps->min_bitpool < SBC_MIN_BITPOOL ? SBC_MIN_BITPOOL :
        caps->min_bitpool;
    max_pool = caps->max_bitpool > LE_A2DP_SRC_SBC_BITPOOL_MAX ?
        LE_A2DP_SRC_SBC_BITPOOL_MAX : caps->max_bitpool;
    if (min_pool > max_pool)
        return -1;

    config->freq_channel = (uint8_t)(freq | chan);
    config->blocks_subbands_alloc = (uint8_t)(blocks | subbands | alloc);
    config->min_bitpool = min_pool;
    config->max_bitpool = max_pool;
    return 0;
}

size_t le_a2dp_src_build_set_config(const struct le_a2dp_src_sbc_config *config,
                                    uint8_t acp_seid, uint8_t int_seid,
                                    uint8_t *out, size_t out_max)
{
    static const uint8_t transport_cap[2] = { AVDTP_CAT_MEDIA_TRANSPORT, 0 };
    size_t offset = 0;

    if (!config || !out || out_max < 2 + sizeof(transport_cap) + 8)
        return 0;
    out[offset++] = (uint8_t)((acp_seid & 0x3f) << 2);
    out[offset++] = (uint8_t)((int_seid & 0x3f) << 2);
    memcpy(out + offset, transport_cap, sizeof(transport_cap));
    offset += sizeof(transport_cap);
    out[offset++] = AVDTP_CAT_MEDIA_CODEC;
    out[offset++] = 6;
    out[offset++] = (uint8_t)(AVDTP_MEDIA_TYPE_AUDIO << 4);
    out[offset++] = AVDTP_CODEC_SBC;
    out[offset++] = config->freq_channel;
    out[offset++] = config->blocks_subbands_alloc;
    out[offset++] = config->min_bitpool;
    out[offset++] = config->max_bitpool;
    return offset;
}

size_t le_a2dp_src_build_seid_payload(uint8_t acp_seid, uint8_t *out,
                                      size_t out_max)
{
    if (!out || out_max < 1)
        return 0;
    out[0] = (uint8_t)((acp_seid & 0x3f) << 2);
    return 1;
}

int le_a2dp_src_encoder_init(sbc_t *encoder,
                             const struct le_a2dp_src_sbc_config *config)
{
    uint8_t freq, chan, blocks, subbands, alloc;

    if (!encoder || !config)
        return -1;
    if (sbc_init(encoder, 0) < 0)
        return -1;

    freq = config->freq_channel & 0xf0;
    chan = config->freq_channel & 0x0f;
    blocks = config->blocks_subbands_alloc & 0xf0;
    subbands = config->blocks_subbands_alloc & 0x0c;
    alloc = config->blocks_subbands_alloc & 0x03;

    encoder->frequency = freq == SBC_FREQ_44K ? SBC_FREQ_44100 : SBC_FREQ_48000;
    switch (chan) {
    case SBC_CHAN_MONO: encoder->mode = SBC_MODE_MONO; break;
    case SBC_CHAN_DUAL: encoder->mode = SBC_MODE_DUAL_CHANNEL; break;
    case SBC_CHAN_STEREO: encoder->mode = SBC_MODE_STEREO; break;
    default: encoder->mode = SBC_MODE_JOINT_STEREO; break;
    }
    switch (blocks) {
    case SBC_BLOCKS_4: encoder->blocks = SBC_BLK_4; break;
    case SBC_BLOCKS_8: encoder->blocks = SBC_BLK_8; break;
    case SBC_BLOCKS_12: encoder->blocks = SBC_BLK_12; break;
    default: encoder->blocks = SBC_BLK_16; break;
    }
    encoder->subbands = subbands == SBC_SUBBANDS_8 ? SBC_SB_8 : SBC_SB_4;
    encoder->allocation = alloc == SBC_ALLOC_SNR ? SBC_AM_SNR : SBC_AM_LOUDNESS;
    encoder->bitpool = config->max_bitpool;
    return 0;
}

size_t le_a2dp_src_rtp_header(uint8_t *out, size_t out_max, uint16_t sequence,
                              uint32_t timestamp, uint32_t ssrc, int marker)
{
    if (!out || out_max < RTP_HEADER_LENGTH)
        return 0;
    out[0] = 0x80; /* version 2, no padding, no extension, no CSRC */
    out[1] = (uint8_t)((marker ? 0x80 : 0x00) | LE_A2DP_SRC_RTP_PAYLOAD_TYPE);
    out[2] = (uint8_t)(sequence >> 8);
    out[3] = (uint8_t)sequence;
    out[4] = (uint8_t)(timestamp >> 24);
    out[5] = (uint8_t)(timestamp >> 16);
    out[6] = (uint8_t)(timestamp >> 8);
    out[7] = (uint8_t)timestamp;
    out[8] = (uint8_t)(ssrc >> 24);
    out[9] = (uint8_t)(ssrc >> 16);
    out[10] = (uint8_t)(ssrc >> 8);
    out[11] = (uint8_t)ssrc;
    return RTP_HEADER_LENGTH;
}

int le_a2dp_src_encode_media_payload(sbc_t *encoder, const uint8_t *pcm,
                                     size_t pcm_length, uint8_t *out,
                                     size_t payload_max, size_t *written,
                                     size_t *consumed)
{
    size_t code_size, frame_size, frames = 0, used = 0, out_used;

    if (!encoder || !pcm || !out || !written || !consumed)
        return -1;
    *written = 0;
    *consumed = 0;
    code_size = sbc_get_codesize(encoder);
    frame_size = sbc_get_frame_length(encoder);
    if (code_size == 0 || frame_size == 0 ||
        payload_max < MEDIA_HEADER_LENGTH + frame_size)
        return -1;

    out_used = MEDIA_HEADER_LENGTH;
    while (frames < LE_A2DP_SRC_MAX_FRAMES_PER_PACKET &&
           pcm_length - used >= code_size &&
           payload_max - out_used >= frame_size) {
        ssize_t frame_written = 0;
        ssize_t frame_consumed;

        frame_consumed = sbc_encode(encoder, pcm + used, code_size,
                                    out + out_used, frame_size,
                                    &frame_written);
        if (frame_consumed < 0 || frame_written <= 0 ||
            (size_t)frame_written > frame_size)
            return -1;
        used += (size_t)frame_consumed;
        out_used += (size_t)frame_written;
        frames++;
    }
    if (frames == 0)
        return 0;
    out[0] = (uint8_t)(MEDIA_HEADER_START_LAST | frames);
    *written = out_used;
    *consumed = used;
    return (int)frames;
}
