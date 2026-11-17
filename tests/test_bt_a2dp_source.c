/*
 * Host regression test for the A2DP source-role protocol core
 * (src/adapter/bt_a2dp_source.c).  Covers AVDTP command and response framing,
 * endpoint and SBC capability selection, SET_CONFIGURATION bytes, the RTP
 * header, and an SBC round trip (encode a test tone, decode it again).  No
 * Bluetooth controller or speaker is needed.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "adapter/bt_a2dp_source.h"

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL: %s\n", what);
        failures++;
    } else {
        printf("ok: %s\n", what);
    }
}

/* Triangle wave, interleaved S16_LE stereo; deterministic and libm-free. */
static void fill_tone(int16_t *pcm, size_t stereo_frames)
{
    size_t i;

    for (i = 0; i < stereo_frames; ++i) {
        int32_t phase = (int32_t)(i % 100);
        int16_t value = (int16_t)((phase < 50 ? phase : 100 - phase) * 300 - 7500);

        pcm[2 * i] = value;
        pcm[2 * i + 1] = value;
    }
}

static void test_command_framing(void)
{
    static const uint8_t discover_expect[2] = { 0x10, 0x01 };
    static const uint8_t set_expect[] = { 0x20, 0x03, 0x04, 0x08 };
    uint8_t packet[64];
    uint8_t payload[2] = { 0x04, 0x08 };
    size_t length;

    length = le_a2dp_src_build_command(1, LE_A2DP_SRC_SIGNAL_DISCOVER, NULL, 0,
                                       packet, sizeof(packet));
    check(length == 2 && !memcmp(packet, discover_expect, 2),
          "DISCOVER command is SINGLE, transaction 1, signal 0x01");

    length = le_a2dp_src_build_command(2, LE_A2DP_SRC_SIGNAL_SET_CONFIGURATION,
                                       payload, 2, packet, sizeof(packet));
    check(length == 4 && !memcmp(packet, set_expect, 4),
          "command payload is appended after the 2-byte header");

    check(le_a2dp_src_build_command(1, 0x01, NULL, 0, packet, 1) == 0,
          "command rejects a buffer shorter than the header");
    check(le_a2dp_src_build_command(1, 0x01, payload, 2, packet, 3) == 0,
          "command rejects a payload that does not fit");
}

static void test_response_parsing(void)
{
    /* ACCEPT (msg 2) for DISCOVER (0x01), transaction 1. */
    static const uint8_t accept[] = { 0x12, 0x01, 0x04, 0x08 };
    /* REJECT (msg 3) for DISCOVER. */
    static const uint8_t reject[] = { 0x13, 0x01, 0x19 };
    /* ACCEPT for a different signal. */
    static const uint8_t other[] = { 0x12, 0x02 };
    const uint8_t *payload = NULL;
    size_t payload_length = 0;

    check(le_a2dp_src_response_payload(accept, sizeof(accept),
                                       LE_A2DP_SRC_SIGNAL_DISCOVER, &payload,
                                       &payload_length) == 0 &&
          payload_length == 2 && payload == accept + 2,
          "ACCEPT for the expected signal returns its payload");
    check(le_a2dp_src_response_payload(reject, sizeof(reject),
                                       LE_A2DP_SRC_SIGNAL_DISCOVER, &payload,
                                       &payload_length) != 0,
          "REJECT is not treated as success");
    check(le_a2dp_src_response_payload(other, sizeof(other),
                                       LE_A2DP_SRC_SIGNAL_DISCOVER, &payload,
                                       &payload_length) != 0,
          "ACCEPT for a different signal is rejected");
    check(le_a2dp_src_response_payload(accept, 1,
                                       LE_A2DP_SRC_SIGNAL_DISCOVER, &payload,
                                       &payload_length) != 0,
          "truncated response header is rejected");
}

static void test_discover_selection(void)
{
    /* SEID 1 in use (bit 1 of byte 0), SEID 2 a source (TSEP clear),
     * SEID 3 an idle audio sink. */
    static const uint8_t endpoints[] = {
        0x06, 0x08, /* SEID 1, in use, sink */
        0x08, 0x00, /* SEID 2, idle, source */
        0x0c, 0x08, /* SEID 3, idle, sink */
    };
    uint8_t seid = 0xff;

    check(le_a2dp_src_find_sink_seid(endpoints, sizeof(endpoints), &seid) == 0 &&
          seid == 3,
          "DISCOVER picks the idle audio sink endpoint, skipping busy/source");
    check(le_a2dp_src_find_sink_seid(endpoints, 4, &seid) != 0,
          "DISCOVER with no idle sink endpoint is an error");
}

/* GET_CAPABILITIES payload: MEDIA_TRANSPORT followed by an SBC codec element
 * with the given codec information bytes (A2DP 4.3.2 bit layout). */
static size_t build_sbc_caps(uint8_t *out, uint8_t freq_channel,
                             uint8_t blocks_sub_alloc, uint8_t min_pool,
                             uint8_t max_pool)
{
    static const uint8_t transport[2] = { 0x01, 0x00 };
    size_t offset = 0;

    memcpy(out, transport, sizeof(transport));
    offset += sizeof(transport);
    out[offset++] = 0x07;
    out[offset++] = 0x06;
    out[offset++] = 0x00; /* audio */
    out[offset++] = 0x00; /* SBC */
    out[offset++] = freq_channel;
    out[offset++] = blocks_sub_alloc;
    out[offset++] = min_pool;
    out[offset++] = max_pool;
    return offset;
}

static void test_caps_and_config(void)
{
    uint8_t caps_payload[32];
    size_t caps_length;
    struct le_a2dp_src_sbc_caps caps;
    struct le_a2dp_src_sbc_config config;
    uint8_t set_payload[32];
    size_t set_length;
    /* Remote offers 44.1k+48k, joint+stereo+mono, 16/8 blocks/subbands,
     * SNR+loudness, bitpool 2..250. */
    uint8_t freq_channel = 0x30 | 0x0b;
    uint8_t blocks_sub_alloc = 0x10 | 0x0c | 0x03;
    static const uint8_t expect_set[] = {
        0x04, 0x04,             /* ACP SEID 1, INT SEID 1 */
        0x01, 0x00,             /* MEDIA_TRANSPORT */
        0x07, 0x06, 0x00, 0x00, /* MEDIA_CODEC, audio, SBC */
        0x11,                   /* 48 kHz | joint stereo */
        0x15,                   /* 16 blocks | 8 subbands | loudness */
        0x02, 0x35,             /* bitpool 2..53 */
    };

    caps_length = build_sbc_caps(caps_payload, freq_channel, blocks_sub_alloc,
                                 2, 250);
    check(le_a2dp_src_parse_sbc_caps(caps_payload, caps_length, &caps) == 0 &&
          caps.freq_channel == freq_channel &&
          caps.max_bitpool == 250,
          "SBC codec element is found behind MEDIA_TRANSPORT");

    check(le_a2dp_src_choose_sbc(&caps, &config) == 0 &&
          config.freq_channel == 0x11 &&
          config.blocks_subbands_alloc == 0x15 &&
          config.min_bitpool == 2 && config.max_bitpool == 53,
          "choice prefers 48 kHz, joint stereo, 16 blocks, 8 subbands, "
          "loudness, bitpool capped at 53");

    set_length = le_a2dp_src_build_set_config(&config, 1, 1, set_payload,
                                              sizeof(set_payload));
    check(set_length == sizeof(expect_set) &&
          !memcmp(set_payload, expect_set, sizeof(expect_set)),
          "SET_CONFIGURATION payload bytes match the chosen SBC configuration");

    caps.freq_channel = 0x20;           /* 44.1 kHz only */
    caps.blocks_subbands_alloc = 0x10;  /* 16 blocks, no subband/alloc bits */
    check(le_a2dp_src_choose_sbc(&caps, &config) != 0,
          "choice fails when a required field has no common value");

    caps.freq_channel = 0x20 | 0x02;
    caps.blocks_subbands_alloc = 0x10 | 0x04 | 0x01;
    caps.min_bitpool = 60;
    caps.max_bitpool = 80;
    check(le_a2dp_src_choose_sbc(&caps, &config) != 0,
          "choice fails when the remote minimum bitpool exceeds 53");

    set_length = le_a2dp_src_build_set_config(&config, 1, 1, set_payload, 4);
    check(set_length == 0,
          "SET_CONFIGURATION rejects a buffer that is too small");

    caps_length = build_sbc_caps(caps_payload, 0x20 | 0x02, 0x10, 2, 53);
    caps_payload[4] = 0x10; /* media type nibble 1 = video, not audio */
    check(le_a2dp_src_parse_sbc_caps(caps_payload, caps_length, &caps) != 0,
          "codec element with a non-audio media type is not accepted");
    caps_payload[4] = 0x00;
    caps_payload[5] = 0x02; /* codec 2 (MPEG-2/4 AAC here), not SBC */
    check(le_a2dp_src_parse_sbc_caps(caps_payload, caps_length, &caps) != 0,
          "codec element that is not SBC is not accepted");
}

static void test_rtp_header(void)
{
    uint8_t header[12];
    static const uint8_t expect[12] = {
        0x80, 0xe0, 0x12, 0x34, 0x00, 0x00, 0x01, 0x00,
        0xde, 0xad, 0xbe, 0xef,
    };

    check(le_a2dp_src_rtp_header(header, sizeof(header), 0x1234, 0x100,
                                 0xdeadbeef, 1) == 12 &&
          !memcmp(header, expect, 12),
          "RTP header: V=2, marker set, PT 96, sequence, timestamp, SSRC");
    check(le_a2dp_src_rtp_header(header, 11, 0, 0, 0, 0) == 0,
          "RTP header rejects a short buffer");
}

static void test_sbc_round_trip(void)
{
    struct le_a2dp_src_sbc_config config = { 0x11, 0x15, 2, 53 };
    sbc_t encoder;
    sbc_t decoder;
    int16_t pcm[2 * 2048];
    uint8_t packet[1 + 15 * 200];
    uint8_t decoded[16 * 512];
    size_t consumed = 0;
    size_t written = 0;
    size_t code_size;
    size_t frame_size;
    int frames;
    size_t decoded_bytes = 0;
    size_t offset = 1;
    int i;

    check(le_a2dp_src_encoder_init(&encoder, &config) == 0,
          "SBC encoder initialises from the chosen configuration");
    code_size = sbc_get_codesize(&encoder);
    frame_size = sbc_get_frame_length(&encoder);
    check(code_size == 512 && frame_size > 4 && frame_size < 200,
          "SBC codesize is 512 bytes (16 blocks x 8 subbands x stereo x S16)");

    fill_tone(pcm, 2048);
    frames = le_a2dp_src_encode_media_payload(&encoder, (const uint8_t *)pcm,
                                              sizeof(pcm), packet,
                                              sizeof(packet), &written,
                                              &consumed);
    check(frames == LE_A2DP_SRC_MAX_FRAMES_PER_PACKET &&
          consumed == (size_t)frames * code_size &&
          written == 1 + (size_t)frames * frame_size,
          "one media packet carries the maximum 15 frames");
    check(packet[0] == (0x60 | frames),
          "media payload header: start and last bits set, frame count 15");
    for (i = 0; i < frames; ++i) {
        if (packet[offset] != 0x9c) {
            break;
        }
        offset += frame_size;
    }
    check(i == frames && offset == written,
          "every encoded frame starts with the SBC syncword 0x9c");

    check(le_a2dp_src_encode_media_payload(&encoder, (const uint8_t *)pcm,
                                           100, packet, sizeof(packet),
                                           &written, &consumed) == 0,
          "less than one PCM frame encodes nothing");

    check(le_a2dp_src_encode_media_payload(&encoder, (const uint8_t *)pcm,
                                           sizeof(pcm), packet, frame_size,
                                           &written, &consumed) == -1,
          "a payload budget smaller than one frame is an error");

    if (sbc_init(&decoder, 0) < 0) {
        check(0, "SBC decoder initialises");
        return;
    }
    decoder.frequency = SBC_FREQ_48000;
    decoder.mode = SBC_MODE_JOINT_STEREO;
    decoder.blocks = SBC_BLK_16;
    decoder.subbands = SBC_SB_8;
    decoder.allocation = SBC_AM_LOUDNESS;
    decoder.bitpool = 53;
    for (i = 0; i < frames; ++i) {
        size_t wrote = 0;
        ssize_t used = sbc_decode(&decoder, packet + 1 + (size_t)i * frame_size,
                                  frame_size, decoded + decoded_bytes,
                                  sizeof(decoded) - decoded_bytes, &wrote);

        if (used < 0 || (size_t)used != frame_size || wrote != code_size)
            break;
        decoded_bytes += wrote;
    }
    check(i == frames && decoded_bytes == (size_t)frames * code_size,
          "encoded frames decode back to full PCM blocks");
    sbc_finish(&decoder);
    sbc_finish(&encoder);
}

int main(void)
{
    test_command_framing();
    test_response_parsing();
    test_discover_selection();
    test_caps_and_config();
    test_rtp_header();
    test_sbc_round_trip();
    if (failures) {
        printf("bt-a2dp-source: %d failure(s)\n", failures);
        return 1;
    }
    printf("bt-a2dp-source: all checks passed\n");
    return 0;
}
