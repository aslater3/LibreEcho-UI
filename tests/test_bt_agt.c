/*
 * Unit test for the AGT (Alexa Gadget Toolkit) codec (wall-clock support).
 *
 * The expected payloads and the framed Discover were captured from a real
 * Echo talking to an Echo Wall Clock, so these assertions pin the exact
 * bytes the clock accepts; it silently ignores anything that differs.
 */
#include "adapter/bt_agt.h"

#include <stdio.h>
#include <string.h>

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static int bytes_equal(const char *label, const uint8_t *actual,
                       size_t actual_length, const uint8_t *expected,
                       size_t expected_length)
{
    size_t i;

    if (actual_length == expected_length &&
        memcmp(actual, expected, expected_length) == 0)
        return 1;
    fprintf(stderr, "%s: got", label);
    for (i = 0; i < actual_length; ++i)
        fprintf(stderr, " %02x", actual[i]);
    fprintf(stderr, "\nexpected");
    for (i = 0; i < expected_length; ++i)
        fprintf(stderr, " %02x", expected[i]);
    fprintf(stderr, "\n");
    return 0;
}

/* Captured from a real Echo Dot (PROTOCOL.md section 5b). */
static const uint8_t DISCOVER[] = {
    0x0a, 0x1d, 0x0a, 0x1b, 0x0a, 0x0f, 0x41, 0x6c, 0x65, 0x78,
    0x61, 0x2e, 0x44, 0x69, 0x73, 0x63, 0x6f, 0x76, 0x65, 0x72,
    0x79, 0x12, 0x08, 0x44, 0x69, 0x73, 0x63, 0x6f, 0x76, 0x65,
    0x72
};

static const uint8_t TIMEINFO[] = {
    0x0a, 0x54, 0x0a, 0x29, 0x0a, 0x1a, 0x41, 0x6c, 0x65, 0x78,
    0x61, 0x2e, 0x47, 0x61, 0x64, 0x67, 0x65, 0x74, 0x2e, 0x53,
    0x74, 0x61, 0x74, 0x65, 0x4c, 0x69, 0x73, 0x74, 0x65, 0x6e,
    0x65, 0x72, 0x12, 0x0b, 0x53, 0x74, 0x61, 0x74, 0x65, 0x55,
    0x70, 0x64, 0x61, 0x74, 0x65, 0x12, 0x27, 0x0a, 0x25, 0x0a,
    0x08, 0x74, 0x69, 0x6d, 0x65, 0x69, 0x6e, 0x66, 0x6f, 0x12,
    0x19, 0x32, 0x30, 0x32, 0x36, 0x2d, 0x31, 0x30, 0x2d, 0x31,
    0x34, 0x54, 0x31, 0x30, 0x3a, 0x33, 0x38, 0x3a, 0x35, 0x32,
    0x2d, 0x30, 0x34, 0x3a, 0x30, 0x30
};

static const uint8_t SETALERT[] = {
    0x0a, 0x44, 0x0a, 0x12, 0x0a, 0x06, 0x41, 0x6c, 0x65, 0x72,
    0x74, 0x73, 0x12, 0x08, 0x53, 0x65, 0x74, 0x41, 0x6c, 0x65,
    0x72, 0x74, 0x12, 0x2e, 0x0a, 0x0a, 0x31, 0x38, 0x37, 0x30,
    0x38, 0x36, 0x32, 0x31, 0x31, 0x35, 0x12, 0x05, 0x54, 0x49,
    0x4d, 0x45, 0x52, 0x1a, 0x19, 0x32, 0x30, 0x32, 0x36, 0x2d,
    0x31, 0x30, 0x2d, 0x31, 0x34, 0x54, 0x31, 0x30, 0x3a, 0x34,
    0x34, 0x3a, 0x31, 0x39, 0x2d, 0x30, 0x34, 0x3a, 0x30, 0x30
};

/* f0 02 00 01 <discover> 09 91 f1 */
static const uint8_t DISCOVER_FRAMED[] = {
    0xf0, 0x02, 0x00, 0x01, 0x0a, 0x1d, 0x0a, 0x1b, 0x0a, 0x0f,
    0x41, 0x6c, 0x65, 0x78, 0x61, 0x2e, 0x44, 0x69, 0x73, 0x63,
    0x6f, 0x76, 0x65, 0x72, 0x79, 0x12, 0x08, 0x44, 0x69, 0x73,
    0x63, 0x6f, 0x76, 0x65, 0x72, 0x09, 0x91, 0xf1
};

static int test_payloads(void)
{
    uint8_t out[LE_AGT_FRAME_MAX];
    size_t length;

    length = le_agt_build_discover(out, sizeof(out));
    CHECK(bytes_equal("discover", out, length, DISCOVER, sizeof(DISCOVER)));

    length = le_agt_build_state_update(out, sizeof(out), "timeinfo",
                                       "2026-10-14T10:38:52-04:00");
    CHECK(bytes_equal("timeinfo", out, length, TIMEINFO, sizeof(TIMEINFO)));

    length = le_agt_build_set_alert(out, sizeof(out), "1870862115", "TIMER",
                                     "2026-10-14T10:44:19-04:00");
    CHECK(bytes_equal("setalert", out, length, SETALERT, sizeof(SETALERT)));
    return 0;
}

static int test_framing(void)
{
    uint8_t payload[LE_AGT_PAYLOAD_MAX];
    uint8_t frame[LE_AGT_FRAME_MAX];
    size_t length;
    size_t consumed = 0;
    int decoded;

    length = le_agt_build_discover(payload, sizeof(payload));
    CHECK(length == sizeof(DISCOVER));

    length = le_agt_encode(frame, sizeof(frame), 1, payload, length);
    CHECK(bytes_equal("framed discover", frame, length, DISCOVER_FRAMED,
                      sizeof(DISCOVER_FRAMED)));

    decoded = le_agt_decode(frame, length, payload, sizeof(payload), &consumed);
    CHECK(decoded == (int)sizeof(DISCOVER));
    CHECK(consumed == length);
    CHECK(memcmp(payload, DISCOVER, sizeof(DISCOVER)) == 0);

    /* A frame followed by another frame consumes only the first. */
    memcpy(frame + length, DISCOVER_FRAMED, sizeof(DISCOVER_FRAMED));
    decoded = le_agt_decode(frame, length + sizeof(DISCOVER_FRAMED), payload,
                            sizeof(payload), &consumed);
    CHECK(decoded == (int)sizeof(DISCOVER));
    CHECK(consumed == length);
    return 0;
}

static int test_escaped_bytes(void)
{
    /* 0xF0/0xF1/0xF2 in the payload must be escaped and round-trip. */
    static const uint8_t payload[] = { 0x01, 0xF0, 0xF1, 0xF2, 0x02 };
    uint8_t frame[LE_AGT_FRAME_MAX];
    uint8_t decoded[LE_AGT_PAYLOAD_MAX];
    size_t length;
    size_t consumed = 0;
    int result;

    length = le_agt_encode(frame, sizeof(frame), 5, payload, sizeof(payload));
    CHECK(length > sizeof(payload) + 5);
    result = le_agt_decode(frame, length, decoded, sizeof(decoded), &consumed);
    CHECK(result == (int)sizeof(payload));
    CHECK(memcmp(decoded, payload, sizeof(payload)) == 0);
    return 0;
}

static int test_corruption(void)
{
    uint8_t frame[LE_AGT_FRAME_MAX];
    uint8_t decoded[LE_AGT_PAYLOAD_MAX];
    size_t length;
    size_t consumed = 0;

    memcpy(frame, DISCOVER_FRAMED, sizeof(DISCOVER_FRAMED));
    length = sizeof(DISCOVER_FRAMED);

    /* Truncated frame. */
    CHECK(le_agt_decode(frame, length - 1, decoded, sizeof(decoded),
                         &consumed) < 0);
    /* Bad checksum. */
    frame[length - 3] ^= 0x01;
    CHECK(le_agt_decode(frame, length, decoded, sizeof(decoded),
                         &consumed) < 0);
    /* Wrong command byte. */
    memcpy(frame, DISCOVER_FRAMED, sizeof(DISCOVER_FRAMED));
    frame[1] = 0x03;
    CHECK(le_agt_decode(frame, length, decoded, sizeof(decoded),
                         &consumed) < 0);
    return 0;
}

static int test_event_header(void)
{
    /*
     * Event{1: Header{1 namespace, 2 name}} for a gadget's
     * Alexa.Discovery / Discover.Response.
     */
    static const uint8_t payload[] = {
        0x0a, 0x26, 0x0a, 0x24, 0x0a, 0x0f,
        0x41, 0x6c, 0x65, 0x78, 0x61, 0x2e, 0x44, 0x69, 0x73, 0x63,
        0x6f, 0x76, 0x65, 0x72, 0x79, 0x12, 0x11,
        0x44, 0x69, 0x73, 0x63, 0x6f, 0x76, 0x65, 0x72, 0x2e, 0x52,
        0x65, 0x73, 0x70, 0x6f, 0x6e, 0x73, 0x65
    };
    char namespace_out[64];
    char name_out[64];

    CHECK(le_agt_read_event_header(payload, sizeof(payload), namespace_out,
                                   sizeof(namespace_out), name_out,
                                   sizeof(name_out)) == 0);
    CHECK(strcmp(namespace_out, "Alexa.Discovery") == 0);
    CHECK(strcmp(name_out, "Discover.Response") == 0);

    /* Truncated payloads are rejected rather than read past their end. */
    CHECK(le_agt_read_event_header(payload, 4, namespace_out,
                                   sizeof(namespace_out), name_out,
                                   sizeof(name_out)) < 0);
    return 0;
}

static int test_bounds(void)
{
    uint8_t small[8];
    uint8_t out[LE_AGT_FRAME_MAX];

    /* A short output buffer must fail, not overrun. */
    CHECK(le_agt_build_discover(small, sizeof(small)) == 0);
    CHECK(le_agt_encode(small, sizeof(small), 1, DISCOVER,
                        sizeof(DISCOVER)) == 0);
    /* NULL arguments are tolerated. */
    CHECK(le_agt_build_state_update(out, sizeof(out), NULL, "x") == 0);
    CHECK(le_agt_build_set_alert(out, sizeof(out), "1", NULL, "t") == 0);
    return 0;
}

/*
 * A frame whose ETX sits inside the four-byte header used to make
 * `end - 4` wrap and sent agt_unescape past its buffer (found with
 * AddressSanitizer).  A malformed frame from the gadget is enough.
 */
static int test_header_etx(void)
{
    uint8_t payload[LE_AGT_PAYLOAD_MAX];
    static const uint8_t inside_end[]   = { 0xF0, 0x02, 0x00, 0xF1, 0x00 };
    static const uint8_t inside_start[] = { 0xF0, 0xF1, 0x00, 0x00, 0x00 };
    static const uint8_t inside_cmd[]   = { 0xF0, 0x02, 0xF1, 0x00, 0x00 };

    CHECK(le_agt_decode(inside_end, sizeof(inside_end), payload,
                        sizeof(payload), NULL) == -1);
    CHECK(le_agt_decode(inside_start, sizeof(inside_start), payload,
                        sizeof(payload), NULL) == -1);
    CHECK(le_agt_decode(inside_cmd, sizeof(inside_cmd), payload,
                        sizeof(payload), NULL) == -1);
    return 0;
}

/*
 * Frames a well-behaved peer cannot produce.  The decoder used to accept all
 * three: a payload past LE_AGT_PAYLOAD_MAX, a raw reserved byte in the body,
 * and an escape that decoded to a non-reserved byte.
 */
static int test_decoder_strictness(void)
{
    uint8_t payload[LE_AGT_PAYLOAD_MAX];
    uint8_t oversized[1024];
    size_t n = 0;
    uint32_t sum = LE_AGT_CMD + LE_AGT_ERR;
    size_t i;

    /* 513-byte payload, valid checksum, otherwise well formed. */
    oversized[n++] = LE_AGT_STX;
    oversized[n++] = LE_AGT_CMD;
    oversized[n++] = LE_AGT_ERR;
    oversized[n++] = 1;
    for (i = 0; i < (size_t)LE_AGT_PAYLOAD_MAX + 1; ++i) {
        oversized[n++] = 0x41;
        sum += 0x41;
    }
    oversized[n++] = (uint8_t)(sum >> 8);
    oversized[n++] = (uint8_t)sum;
    oversized[n++] = LE_AGT_ETX;
    CHECK(le_agt_decode(oversized, n, payload, sizeof(payload), NULL) == -1);

    /* A raw STX inside the body (the encoder always escapes it). */
    {
        static const uint8_t raw_stx[] = {
            0xF0, 0x02, 0x00, 0x01, 0xF0, 0x00, 0xF2, 0x00, 0xF1
        };
        CHECK(le_agt_decode(raw_stx, sizeof(raw_stx), payload,
                            sizeof(payload), NULL) == -1);
    }

    /* ESC + a non-reserved byte: two wire forms for one payload. */
    {
        static const uint8_t non_canonical[] = {
            0xF0, 0x02, 0x00, 0x01, 0x41, 0x42, 0x43, 0xF2, 0x00, 0xF1
        };
        CHECK(le_agt_decode(non_canonical, sizeof(non_canonical), payload,
                            sizeof(payload), NULL) == -1);
    }
    return 0;
}

/*
 * A crafted inner length near 2^32 used to wrap `offset + field_length` in
 * pb_read_bytes on a 32-bit target, so the bounds check passed and the caller
 * received a pointer with a bogus length.  A single malformed frame from the
 * gadget then read out of bounds (SIGSEGV on the device).
 */
static int test_length_wrap(void)
{
    /* outer Message{1: bytes, len 8}; inner Event{1: bytes, len 0xFFFFFFFA} */
    static const uint8_t crafted[] = {
        0x0A, 0x08,
        0x0A, 0xFA, 0xFF, 0xFF, 0xFF, 0x0F,
        0x0A, 0x1D
    };
    char ns[32];
    char name[32];

    ns[0] = 'X';
    name[0] = 'X';
    CHECK(le_agt_read_event_header(crafted, sizeof(crafted), ns, sizeof(ns),
                                   name, sizeof(name)) == -1);
    return 0;
}

int main(void)
{
    if (test_payloads() || test_framing() || test_escaped_bytes() ||
        test_corruption() || test_event_header() || test_bounds() ||
        test_header_etx() || test_length_wrap() || test_decoder_strictness())
        return 1;
    printf("bt agt codec contract: ok\n");
    return 0;
}
