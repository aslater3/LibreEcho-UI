/*
 * Alexa Gadget Toolkit (AGT) framing and protobuf codec.
 *
 * See bt_agt.h for the wire format.  The protobuf subset is deliberately
 * tiny: the AGT messages the wall clock needs use only length-delimited and
 * varint fields, so a full protobuf runtime is not worth carrying.
 */
#include "adapter/bt_agt.h"

#include <string.h>

#ifdef __GNUC__
#define AGT_LIKELY(x)   __builtin_expect(!!(x), 1)
#define AGT_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define AGT_COLD        __attribute__((cold))
#else
#define AGT_LIKELY(x)   (x)
#define AGT_UNLIKELY(x) (x)
#define AGT_COLD
#endif

/* ---- protobuf writer helpers ------------------------------------------- */

/*
 * Bounded string length: stops after `max + 1` bytes so an overlong string
 * cannot force an unbounded scan before overflowing a fixed-size message.
 */
static size_t agt_strnlen(const char *s, size_t max)
{
    size_t n = 0;

    while (n <= max && s[n] != '\0')
        ++n;
    return n;
}

/* Encoded size of a length-delimited field with tag < 16 and len <= 16383. */
static inline size_t pb_field_size(size_t length)
{
    return 1U + (length >= 0x80U ? 2U : 1U) + length;
}

static inline uint8_t *pb_put_varint(uint8_t *dst, size_t value)
{
    if (AGT_LIKELY(value < 0x80U)) {
        *dst++ = (uint8_t)value;
    } else {
        *dst++ = (uint8_t)((value & 0x7FU) | 0x80U);
        *dst++ = (uint8_t)(value >> 7);
    }
    return dst;
}

static inline uint8_t *pb_put_prefix(uint8_t *dst, uint8_t tag, size_t length)
{
    *dst++ = tag;
    return pb_put_varint(dst, length);
}

static uint8_t *pb_put_field(uint8_t *dst, uint8_t tag,
                             const void *value, size_t length)
{
    dst = pb_put_prefix(dst, tag, length);
    if (length)
        memcpy(dst, value, length);
    return dst + length;
}

/* ---- protobuf reader --------------------------------------------------- */

struct pb_reader {
    const uint8_t *ptr;
    size_t rem;
};

static AGT_COLD int pb_read_varint_slow(struct pb_reader *r, uint8_t first,
                                        uint32_t *value)
{
    uint32_t result = (uint32_t)(first & 0x7FU);
    unsigned shift = 7;

    for (;;) {
        uint8_t byte;
        if (r->rem == 0 || shift > 28)
            return -1;
        byte = *r->ptr++;
        --r->rem;
        result |= (uint32_t)(byte & 0x7FU) << shift;
        if (!(byte & 0x80U))
            break;
        shift += 7;
    }
    *value = result;
    return 0;
}

/* Read one varint (inline fast path for the common 1-byte < 128 case). */
static inline int pb_read_varint(struct pb_reader *r, uint32_t *value)
{
    uint8_t byte;

    if (AGT_UNLIKELY(r->rem == 0))
        return -1;
    byte = *r->ptr++;
    --r->rem;
    if (AGT_LIKELY(byte < 0x80U)) {
        *value = byte;
        return 0;
    }
    return pb_read_varint_slow(r, byte, value);
}

/* Read a length-delimited field.  Returns 0 on success. */
static inline int pb_read_bytes(struct pb_reader *r, const uint8_t **value,
                                size_t *length)
{
    uint32_t field_length;

    if (AGT_UNLIKELY(pb_read_varint(r, &field_length) != 0))
        return -1;
    /*
     * Compare directly against remaining bytes rather than adding
     * `offset + field_length`, which wraps on 32-bit targets for a crafted
     * length near 2^32 (see regression test in tests/test_bt_agt.c).
     */
    if (AGT_UNLIKELY(field_length > r->rem))
        return -1;
    *value = r->ptr;
    *length = field_length;
    r->ptr += field_length;
    r->rem -= field_length;
    return 0;
}

/* ---- framing ----------------------------------------------------------- */

static inline int agt_is_reserved(uint8_t byte)
{
    /* STX=0xF0, ETX=0xF1, ESC=0xF2 form a contiguous 3-byte range. */
    return (uint8_t)(byte - LE_AGT_STX) <= 2U;
}

static inline uint8_t *agt_put_escaped(uint8_t *dst, uint8_t byte)
{
    if (AGT_UNLIKELY(agt_is_reserved(byte))) {
        *dst++ = LE_AGT_ESC;
        byte = (uint8_t)(byte ^ LE_AGT_ESC);
    }
    *dst++ = byte;
    return dst;
}

/* Cold path: exact wire length for a tight output buffer (no partial writes). */
static AGT_COLD size_t agt_encoded_length(const uint8_t *payload,
                                          size_t payload_length)
{
    uint32_t sum = (uint32_t)LE_AGT_CMD + LE_AGT_ERR;
    size_t needed = payload_length + 7U;
    size_t i;

    for (i = 0; i < payload_length; ++i) {
        uint8_t b = payload[i];
        sum += b;
        if (agt_is_reserved(b))
            ++needed;
    }
    if (agt_is_reserved((uint8_t)(sum >> 8)))
        ++needed;
    if (agt_is_reserved((uint8_t)sum))
        ++needed;
    return needed;
}

size_t le_agt_encode(uint8_t *out, size_t out_size, uint8_t seq,
                     const uint8_t *payload, size_t payload_length)
{
    uint32_t checksum = (uint32_t)LE_AGT_CMD + LE_AGT_ERR;
    const uint8_t *src;
    const uint8_t *src_end;
    uint8_t *dst;

    if (!out || !payload || payload_length > LE_AGT_PAYLOAD_MAX ||
        agt_is_reserved(seq))
        return 0;
    if (out_size < payload_length + 7U)
        return 0;
    if (AGT_UNLIKELY(out_size < payload_length * 2U + 9U)) {
        if (agt_encoded_length(payload, payload_length) > out_size)
            return 0;
    }

    dst = out;
    *dst++ = LE_AGT_STX;
    *dst++ = LE_AGT_CMD;
    *dst++ = LE_AGT_ERR;
    *dst++ = seq;

    src = payload;
    src_end = payload + payload_length;
    while (src < src_end) {
        uint8_t byte = *src++;
        checksum += byte;
        dst = agt_put_escaped(dst, byte);
    }
    dst = agt_put_escaped(dst, (uint8_t)(checksum >> 8));
    dst = agt_put_escaped(dst, (uint8_t)checksum);
    *dst++ = LE_AGT_ETX;
    return (size_t)(dst - out);
}

int le_agt_decode(const uint8_t *in, size_t in_length, uint8_t *payload,
                  size_t payload_size, size_t *consumed)
{
    uint8_t decoded[LE_AGT_PAYLOAD_MAX + 2];
    const uint8_t *src;
    const uint8_t *src_end;
    uint8_t *dst = decoded;
    uint8_t *dst_end = decoded + sizeof(decoded);
    uint32_t checksum = (uint32_t)LE_AGT_CMD + LE_AGT_ERR;
    uint8_t cksum_hi;
    uint8_t cksum_lo;
    size_t body;

    /*
     * Validate the four-byte header up front.  Rejecting reserved bytes in
     * `seq` (STX/ETX/ESC) preserves the `end < 4` guard against an ETX inside
     * the header and prevents a malformed `seq == ESC` from swallowing an ETX
     * at `in[4]`.
     */
    if (!in || in_length < 7U || in[0] != LE_AGT_STX ||
        in[1] != LE_AGT_CMD || in[2] != LE_AGT_ERR || agt_is_reserved(in[3]))
        return -1;

    /* Single pass: scan to ETX while unescaping and accumulating checksum. */
    src = in + 4;
    src_end = in + in_length;
    for (;;) {
        uint8_t byte;

        if (AGT_UNLIKELY(src >= src_end))
            return -1;
        byte = *src++;
        if (AGT_UNLIKELY(agt_is_reserved(byte))) {
            if (byte == LE_AGT_ETX)
                break;
            if (byte != LE_AGT_ESC || src >= src_end)
                return -1; /* raw STX in body or stray ESC at EOF */
            byte = (uint8_t)(*src++ ^ LE_AGT_ESC);
            /*
             * The encoder only escapes reserved bytes, so an escape that
             * decodes to a non-reserved one is not canonical.
             */
            if (!agt_is_reserved(byte))
                return -1;
        }
        if (AGT_UNLIKELY(dst >= dst_end))
            return -1;
        *dst++ = byte;
        checksum += byte;
    }

    if ((size_t)(dst - decoded) < 2U)
        return -1;
    body = (size_t)(dst - decoded) - 2U;
    cksum_hi = decoded[body];
    cksum_lo = decoded[body + 1U];
    checksum = (checksum - cksum_hi - cksum_lo) & 0xFFFFU;
    if (cksum_hi != (uint8_t)(checksum >> 8) || cksum_lo != (uint8_t)checksum)
        return -1;
    if (body > payload_size)
        return -1;
    if (payload && body)
        memcpy(payload, decoded, body);
    /* Set on success only, so a caller can leave it at a sentinel and tell
     * "rejected" from "consumed 0". */
    if (consumed)
        *consumed = (size_t)(src - in);
    return (int)body;
}

/* ---- directive builders ------------------------------------------------ */

/*
 * Constant directive messages (Message{1: Directive{1: Header, 2?: payload}})
 * and pre-encoded Header{1: namespace, 2: name} prefixes for dynamic builders.
 * Computing exact wire lengths up front lets every builder write once straight
 * into `out` with zero scratch buffers and zero partial writes on overflow.
 */
static const uint8_t AGT_MSG_DISCOVER[] = {
    0x0a, 0x1d, 0x0a, 0x1b, 0x0a, 0x0f,
    'A', 'l', 'e', 'x', 'a', '.', 'D', 'i', 's', 'c', 'o', 'v', 'e', 'r', 'y',
    0x12, 0x08, 'D', 'i', 's', 'c', 'o', 'v', 'e', 'r'
};

static const uint8_t AGT_MSG_CLEAR_INDICATOR[] = {
    0x0a, 0x21, 0x0a, 0x1f, 0x0a, 0x0d,
    'N', 'o', 't', 'i', 'f', 'i', 'c', 'a', 't', 'i', 'o', 'n', 's',
    0x12, 0x0e, 'C', 'l', 'e', 'a', 'r', 'I', 'n', 'd', 'i', 'c', 'a', 't', 'o', 'r'
};

static const uint8_t AGT_MSG_SET_INDICATOR[] = {
    0x0a, 0x23, 0x0a, 0x1d, 0x0a, 0x0d,
    'N', 'o', 't', 'i', 'f', 'i', 'c', 'a', 't', 'i', 'o', 'n', 's',
    0x12, 0x0c, 'S', 'e', 't', 'I', 'n', 'd', 'i', 'c', 'a', 't', 'o', 'r',
    0x12, 0x02, 0x08, 0x01 /* persistVisualIndicator = true */
};

static const uint8_t AGT_HDR_STATE_UPDATE[] = {
    0x0a, 0x29, 0x0a, 0x1a,
    'A', 'l', 'e', 'x', 'a', '.', 'G', 'a', 'd', 'g', 'e', 't', '.',
    'S', 't', 'a', 't', 'e', 'L', 'i', 's', 't', 'e', 'n', 'e', 'r',
    0x12, 0x0b, 'S', 't', 'a', 't', 'e', 'U', 'p', 'd', 'a', 't', 'e'
};

static const uint8_t AGT_HDR_SET_ALERT[] = {
    0x0a, 0x12, 0x0a, 0x06, 'A', 'l', 'e', 'r', 't', 's',
    0x12, 0x08, 'S', 'e', 't', 'A', 'l', 'e', 'r', 't'
};

static const uint8_t AGT_HDR_DELETE_ALERT[] = {
    0x0a, 0x15, 0x0a, 0x06, 'A', 'l', 'e', 'r', 't', 's',
    0x12, 0x0b, 'D', 'e', 'l', 'e', 't', 'e', 'A', 'l', 'e', 'r', 't'
};

static size_t agt_copy_const(uint8_t *out, size_t out_size,
                             const uint8_t *msg, size_t msg_length)
{
    if (!out || out_size < msg_length)
        return 0;
    memcpy(out, msg, msg_length);
    return msg_length;
}

size_t le_agt_build_discover(uint8_t *out, size_t out_size)
{
    return agt_copy_const(out, out_size, AGT_MSG_DISCOVER,
                          sizeof(AGT_MSG_DISCOVER));
}

size_t le_agt_build_state_update(uint8_t *out, size_t out_size,
                                 const char *name, const char *value)
{
    enum { STATE_MAX = 96 };
    size_t name_len;
    size_t value_len;
    size_t state_len;
    size_t payload_len;
    size_t dir_len;
    size_t msg_len;
    uint8_t *dst;

    if (!out || !name || !value)
        return 0;
    name_len = agt_strnlen(name, STATE_MAX);
    value_len = agt_strnlen(value, STATE_MAX);
    if (name_len > STATE_MAX || value_len > STATE_MAX)
        return 0;
    /* Both lengths <= 96 < 128, so their length varints are 1 byte each. */
    state_len = (2U + name_len) + (2U + value_len);
    if (state_len > STATE_MAX)
        return 0;
    payload_len = 2U + state_len;
    dir_len = sizeof(AGT_HDR_STATE_UPDATE) + 2U + payload_len;
    msg_len = pb_field_size(dir_len);
    if (msg_len > out_size)
        return 0;

    dst = pb_put_prefix(out, 0x0A, dir_len);
    memcpy(dst, AGT_HDR_STATE_UPDATE, sizeof(AGT_HDR_STATE_UPDATE));
    dst += sizeof(AGT_HDR_STATE_UPDATE);
    *dst++ = 0x12;
    *dst++ = (uint8_t)payload_len;
    *dst++ = 0x0A;
    *dst++ = (uint8_t)state_len;
    dst = pb_put_field(dst, 0x0A, name, name_len);
    dst = pb_put_field(dst, 0x12, value, value_len);
    return msg_len;
}

size_t le_agt_build_clear_indicator(uint8_t *out, size_t out_size)
{
    return agt_copy_const(out, out_size, AGT_MSG_CLEAR_INDICATOR,
                          sizeof(AGT_MSG_CLEAR_INDICATOR));
}

size_t le_agt_build_set_indicator(uint8_t *out, size_t out_size)
{
    return agt_copy_const(out, out_size, AGT_MSG_SET_INDICATOR,
                          sizeof(AGT_MSG_SET_INDICATOR));
}

size_t le_agt_build_set_alert(uint8_t *out, size_t out_size, const char *token,
                              const char *alert_type,
                              const char *scheduled_time)
{
    size_t token_len;
    size_t type_len;
    size_t time_len;
    size_t payload_len;
    size_t dir_len;
    size_t msg_len;
    uint8_t *dst;

    if (!out || !token || !alert_type || !scheduled_time)
        return 0;
    token_len = agt_strnlen(token, LE_AGT_PAYLOAD_MAX);
    type_len = agt_strnlen(alert_type, LE_AGT_PAYLOAD_MAX);
    time_len = agt_strnlen(scheduled_time, LE_AGT_PAYLOAD_MAX);
    if (token_len > LE_AGT_PAYLOAD_MAX || type_len > LE_AGT_PAYLOAD_MAX ||
        time_len > LE_AGT_PAYLOAD_MAX)
        return 0;
    payload_len = pb_field_size(token_len) + pb_field_size(type_len) +
                  pb_field_size(time_len);
    if (payload_len > LE_AGT_PAYLOAD_MAX)
        return 0;
    dir_len = sizeof(AGT_HDR_SET_ALERT) + pb_field_size(payload_len);
    if (dir_len > LE_AGT_PAYLOAD_MAX)
        return 0;
    msg_len = pb_field_size(dir_len);
    if (msg_len > out_size)
        return 0;

    dst = pb_put_prefix(out, 0x0A, dir_len);
    memcpy(dst, AGT_HDR_SET_ALERT, sizeof(AGT_HDR_SET_ALERT));
    dst += sizeof(AGT_HDR_SET_ALERT);
    dst = pb_put_prefix(dst, 0x12, payload_len);
    dst = pb_put_field(dst, 0x0A, token, token_len);
    dst = pb_put_field(dst, 0x12, alert_type, type_len);
    dst = pb_put_field(dst, 0x1A, scheduled_time, time_len);
    return msg_len;
}

size_t le_agt_build_delete_alert(uint8_t *out, size_t out_size,
                                 const char *token)
{
    enum { DELETE_PAYLOAD_MAX = 128 };
    size_t token_len;
    size_t payload_len;
    size_t dir_len;
    size_t msg_len;
    uint8_t *dst;

    if (!out || !token)
        return 0;
    token_len = agt_strnlen(token, DELETE_PAYLOAD_MAX);
    if (token_len > DELETE_PAYLOAD_MAX)
        return 0;
    payload_len = pb_field_size(token_len);
    if (payload_len > DELETE_PAYLOAD_MAX)
        return 0;
    dir_len = sizeof(AGT_HDR_DELETE_ALERT) + pb_field_size(payload_len);
    msg_len = pb_field_size(dir_len);
    if (msg_len > out_size)
        return 0;

    dst = pb_put_prefix(out, 0x0A, dir_len);
    memcpy(dst, AGT_HDR_DELETE_ALERT, sizeof(AGT_HDR_DELETE_ALERT));
    dst += sizeof(AGT_HDR_DELETE_ALERT);
    dst = pb_put_prefix(dst, 0x12, payload_len);
    dst = pb_put_field(dst, 0x0A, token, token_len);
    return msg_len;
}

/* Copy a length-delimited header string out of a parsed field. */
static void copy_field(const uint8_t *data, size_t length, char *out,
                       size_t out_size)
{
    size_t copy;

    if (!out || out_size == 0)
        return;
    copy = length < out_size - 1 ? length : out_size - 1;
    if (copy)
        memcpy(out, data, copy);
    out[copy] = '\0';
}

int le_agt_read_event_header(const uint8_t *payload, size_t payload_length,
                             char *namespace_out, size_t namespace_size,
                             char *name_out, size_t name_size)
{
    struct pb_reader outer;
    const uint8_t *message;
    size_t message_length;
    struct pb_reader event;
    const uint8_t *event_body;
    size_t event_length;
    struct pb_reader header;
    const uint8_t *ns_data = NULL;
    size_t ns_length = 0;
    const uint8_t *nm_data = NULL;
    size_t nm_length = 0;
    uint32_t tag;

    if (!payload || payload_length == 0)
        return -1;

    /* Message{1: bytes} -> Event{1: Header}. */
    outer.ptr = payload;
    outer.rem = payload_length;
    if (pb_read_varint(&outer, &tag) != 0 || tag != 0x0AU)
        return -1;
    if (pb_read_bytes(&outer, &message, &message_length) != 0)
        return -1;

    event.ptr = message;
    event.rem = message_length;
    if (pb_read_varint(&event, &tag) != 0 || tag != 0x0AU)
        return -1;
    if (pb_read_bytes(&event, &event_body, &event_length) != 0)
        return -1;

    header.ptr = event_body;
    header.rem = event_length;
    while (header.rem > 0) {
        const uint8_t *value;
        size_t value_length;

        if (pb_read_varint(&header, &tag) != 0)
            return -1;
        if ((tag & 7U) == 0U) {
            uint32_t ignored;
            if (pb_read_varint(&header, &ignored) != 0)
                return -1;
            continue;
        }
        if ((tag & 7U) != 2U)
            return -1;
        if (pb_read_bytes(&header, &value, &value_length) != 0)
            return -1;
        if ((tag >> 3) == 1U) {
            ns_data = value;
            ns_length = value_length;
        } else if ((tag >> 3) == 2U) {
            nm_data = value;
            nm_length = value_length;
        }
    }
    if (!ns_data || !nm_data)
        return -1;
    /* Copy only after full validation so caller buffers stay untouched on -1. */
    copy_field(ns_data, ns_length, namespace_out, namespace_size);
    copy_field(nm_data, nm_length, name_out, name_size);
    return 0;
}
