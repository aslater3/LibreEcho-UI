/*
 * LibreEcho A2DP source-role protocol core (issue #411, first slice).
 *
 * Pure AVDTP/SBC/RTP helpers for the initiator (source) side of A2DP:
 * command framing, response parsing, SBC capability negotiation, SBC encoding
 * and RTP media packetization.  No sockets, no daemon state: btd is not wired
 * to this module yet, so nothing here runs on a device.  The byte layouts
 * follow AVDTP 1.3 section 8 and A2DP 1.3 section 4.3 (SBC codec information
 * element) and are exercised by tests/test_bt_a2dp_source.c.
 *
 * SBC codec information element bit positions (A2DP 4.3.2):
 *   byte 2  sampling frequency  16k 0x80, 32k 0x40, 44.1k 0x20, 48k 0x10
 *           channel mode        mono 0x08, dual 0x04, stereo 0x02, joint 0x01
 *   byte 3  block length        4 0x80, 8 0x40, 12 0x20, 16 0x10
 *           subbands            4 0x08, 8 0x04
 *           allocation          SNR 0x02, loudness 0x01
 *   byte 4  minimum bitpool
 *   byte 5  maximum bitpool
 */
#ifndef LIBREECHO_BT_A2DP_SOURCE_H
#define LIBREECHO_BT_A2DP_SOURCE_H

#include <stddef.h>
#include <stdint.h>

#include "adapter/bt-sbc/sbc.h"

#define LE_A2DP_SRC_SIGNAL_DISCOVER 0x01
#define LE_A2DP_SRC_SIGNAL_GET_CAPABILITIES 0x02
#define LE_A2DP_SRC_SIGNAL_SET_CONFIGURATION 0x03
#define LE_A2DP_SRC_SIGNAL_OPEN 0x06
#define LE_A2DP_SRC_SIGNAL_START 0x07
#define LE_A2DP_SRC_SIGNAL_SUSPEND 0x09

/* Largest SBC bitpool the A2DP sink profile accepts (A2DP 4.3.2). */
#define LE_A2DP_SRC_SBC_BITPOOL_MAX 53
/* Dynamic RTP payload type used for the media stream. */
#define LE_A2DP_SRC_RTP_PAYLOAD_TYPE 96
/* Largest SBC frame count one media packet header can describe. */
#define LE_A2DP_SRC_MAX_FRAMES_PER_PACKET 15

struct le_a2dp_src_sbc_caps {
    uint8_t freq_channel;        /* SBC codec information byte 2 */
    uint8_t blocks_subbands_alloc; /* SBC codec information byte 3 */
    uint8_t min_bitpool;
    uint8_t max_bitpool;
};

struct le_a2dp_src_sbc_config {
    uint8_t freq_channel;        /* chosen single bit per field */
    uint8_t blocks_subbands_alloc;
    uint8_t min_bitpool;
    uint8_t max_bitpool;
};

/*
 * Build one AVDTP SINGLE command packet (2-byte header + payload).  Returns
 * the packet length, or 0 when out_max is too small or the payload is too
 * large for a single packet.
 */
size_t le_a2dp_src_build_command(uint8_t transaction, uint8_t signal_id,
                                 const uint8_t *payload, size_t payload_length,
                                 uint8_t *out, size_t out_max);

/*
 * Validate a SINGLE ACCEPT response for signal_id and return its payload.
 * Returns 0 on ACCEPT; -1 for REJECT, GENERAL_REJECT, a different signal,
 * or a malformed header.
 */
int le_a2dp_src_response_payload(const uint8_t *packet, size_t length,
                                 uint8_t signal_id, const uint8_t **payload,
                                 size_t *payload_length);

/*
 * Pick the first idle audio sink endpoint from a DISCOVER response payload.
 * Returns 0 and writes its SEID, or -1 when there is none.
 */
int le_a2dp_src_find_sink_seid(const uint8_t *payload, size_t length,
                               uint8_t *seid);

/*
 * Extract the SBC codec information element from a GET_CAPABILITIES response
 * payload.  Returns 0 when an audio/SBC media codec capability is present.
 */
int le_a2dp_src_parse_sbc_caps(const uint8_t *payload, size_t length,
                               struct le_a2dp_src_sbc_caps *caps);

/*
 * Choose the preferred SBC configuration the remote endpoint supports:
 * 48 kHz over 44.1 kHz, joint stereo over stereo over mono, 16 blocks,
 * 8 subbands, loudness allocation, and the highest bitpool up to 53.
 * Returns 0, or -1 when a required field has no common value.
 */
int le_a2dp_src_choose_sbc(const struct le_a2dp_src_sbc_caps *caps,
                           struct le_a2dp_src_sbc_config *config);

/*
 * Write a SET_CONFIGURATION payload (ACP SEID, INT SEID, transport and SBC
 * codec capabilities).  Returns the payload length or 0 on error.
 */
size_t le_a2dp_src_build_set_config(const struct le_a2dp_src_sbc_config *config,
                                    uint8_t acp_seid, uint8_t int_seid,
                                    uint8_t *out, size_t out_max);

/* Build a one-byte-SEID payload (OPEN, START, SUSPEND). */
size_t le_a2dp_src_build_seid_payload(uint8_t acp_seid, uint8_t *out,
                                      size_t out_max);

/* Initialise an SBC encoder with the chosen configuration. */
int le_a2dp_src_encoder_init(sbc_t *encoder,
                             const struct le_a2dp_src_sbc_config *config);

/* Write the 12-byte RTP header.  Returns 12, or 0 when out_max is short. */
size_t le_a2dp_src_rtp_header(uint8_t *out, size_t out_max, uint16_t sequence,
                              uint32_t timestamp, uint32_t ssrc, int marker);

/*
 * Encode as many whole SBC frames from pcm (S16_LE stereo, codesize bytes per
 * frame) as fit one media packet payload of at most payload_max bytes
 * (including the one-byte media payload header) and at most
 * LE_A2DP_SRC_MAX_FRAMES_PER_PACKET frames.  Writes the header plus frames to
 * out, sets *consumed to the PCM bytes used, and returns the frame count, or
 * -1 on encoder failure.  Returns 0 when pcm holds less than one frame.
 */
int le_a2dp_src_encode_media_payload(sbc_t *encoder, const uint8_t *pcm,
                                     size_t pcm_length, uint8_t *out,
                                     size_t payload_max, size_t *written,
                                     size_t *consumed);

#endif /* LIBREECHO_BT_A2DP_SOURCE_H */
