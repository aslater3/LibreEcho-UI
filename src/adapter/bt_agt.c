/*
 * Alexa Gadget Toolkit (AGT) framing and protobuf codec.
 *
 * See bt_agt.h for the wire format.  The protobuf subset is deliberately
 * tiny: the AGT messages the wall clock needs use only length-delimited and
 * varint fields, so a full protobuf runtime is not worth carrying.
 */
#include "adapter/bt_agt.h"

#include <string.h>

/* ---- protobuf writer --------------------------------------------------- */

struct pb_writer {
    uint8_t *data;
    size_t size;
    size_t used;
    int overflow;
};

static void pb_byte(struct pb_writer *w, uint8_t value)
{
    if (w->used >= w->size)
        w->overflow = 1;
    else
        w->data[w->used++] = value;
}

static void pb_varint(struct pb_writer *w, uint32_t value)
{
    do {
        uint8_t byte = (uint8_t)(value & 0x7FU);
        value >>= 7;
        if (value)
            byte |= 0x80U;
        pb_byte(w, byte);
    } while (value);
}

static void pb_tag(struct pb_writer *w, uint32_t field, uint32_t wire)
{
    pb_varint(w, (field << 3) | wire);
}

static void pb_bytes(struct pb_writer *w, uint32_t field,
                     const uint8_t *value, size_t length)
{
    if (w->overflow)
        return;
    pb_tag(w, field, 2);
    pb_varint(w, (uint32_t)length);
    if (w->used + length > w->size) {
        w->overflow = 1;
        return;
    }
    memcpy(w->data + w->used, value, length);
    w->used += length;
}

static void pb_string(struct pb_writer *w, uint32_t field, const char *value)
{
    pb_bytes(w, field, (const uint8_t *)value, strlen(value));
}

/* Write `inner` as a nested message in field `field`. */
static void pb_message(struct pb_writer *w, uint32_t field,
                       const uint8_t *inner, size_t inner_length)
{
    pb_bytes(w, field, inner, inner_length);
}

/* ---- protobuf reader --------------------------------------------------- */

struct pb_reader {
    const uint8_t *data;
    size_t length;
    size_t offset;
};

/* Read one varint.  Returns 0 on success. */
static int pb_read_varint(struct pb_reader *r, uint32_t *value)
{
    uint32_t result = 0;
    unsigned shift = 0;

    for (;;) {
        uint8_t byte;
        if (r->offset >= r->length || shift > 28)
            return -1;
        byte = r->data[r->offset++];
        result |= (uint32_t)(byte & 0x7FU) << shift;
        if (!(byte & 0x80U))
            break;
        shift += 7;
    }
    *value = result;
    return 0;
}

/* Read a length-delimited field.  Returns 0 on success. */
static int pb_read_bytes(struct pb_reader *r, const uint8_t **value,
                         size_t *length)
{
    uint32_t field_length;

    if (pb_read_varint(r, &field_length) != 0)
        return -1;
    if (r->offset + field_length > r->length)
        return -1;
    *value = r->data + r->offset;
    *length = field_length;
    r->offset += field_length;
    return 0;
}

/* ---- framing ----------------------------------------------------------- */

static int agt_is_reserved(uint8_t byte)
{
    return byte == LE_AGT_STX || byte == LE_AGT_ETX || byte == LE_AGT_ESC;
}

size_t le_agt_encode(uint8_t *out, size_t out_size, uint8_t seq,
                     const uint8_t *payload, size_t payload_length)
{
    uint32_t checksum;
    size_t used = 0;
    size_t i;

    if (!out || !payload || payload_length > LE_AGT_PAYLOAD_MAX)
        return 0;
    checksum = (uint32_t)LE_AGT_CMD + LE_AGT_ERR;
    for (i = 0; i < payload_length; ++i)
        checksum += payload[i];
    checksum &= 0xFFFFU;

    if (out_size < 5)
        return 0;
    out[used++] = LE_AGT_STX;
    out[used++] = LE_AGT_CMD;
    out[used++] = LE_AGT_ERR;
    out[used++] = seq;

    /* Payload then the 16-bit big-endian checksum, both escaped. */
    for (i = 0; i < payload_length + 2; ++i) {
        uint8_t byte = i < payload_length
            ? payload[i]
            : (uint8_t)(i == payload_length ? (checksum >> 8) : checksum);

        if (agt_is_reserved(byte)) {
            if (used + 2 > out_size)
                return 0;
            out[used++] = LE_AGT_ESC;
            out[used++] = (uint8_t)(byte ^ LE_AGT_ESC);
        } else {
            if (used + 1 > out_size)
                return 0;
            out[used++] = byte;
        }
    }
    if (used + 1 > out_size)
        return 0;
    out[used++] = LE_AGT_ETX;
    return used;
}

/* Unescape the bytes between seq and ETX.  Returns the decoded length. */
static int agt_unescape(const uint8_t *in, size_t in_length, uint8_t *out,
                        size_t out_size)
{
    size_t used = 0;
    size_t i = 0;

    while (i < in_length) {
        uint8_t byte = in[i++];

        if (byte == LE_AGT_ESC) {
            if (i >= in_length || used >= out_size)
                return -1;
            byte = (uint8_t)(in[i++] ^ LE_AGT_ESC);
        }
        if (used >= out_size)
            return -1;
        out[used++] = byte;
    }
    return (int)used;
}

int le_agt_decode(const uint8_t *in, size_t in_length, uint8_t *payload,
                  size_t payload_size, size_t *consumed)
{
    size_t end;
    size_t escaped_length;
    uint8_t decoded[LE_AGT_FRAME_MAX];
    int decoded_length;
    uint32_t checksum = 0;
    size_t i;

    if (!in || in_length < 5 || in[0] != LE_AGT_STX)
        return -1;
    /* The frame ends at the first ETX outside an escape pair. */
    for (end = 1; end < in_length; ++end) {
        if (in[end] == LE_AGT_ETX)
            break;
        if (in[end] == LE_AGT_ESC) {
            if (end + 1 >= in_length)
                return -1;
            ++end;
        }
    }
    if (end >= in_length)
        return -1;
    if (in[1] != LE_AGT_CMD || in[2] != LE_AGT_ERR)
        return -1;
    /* The header is STX CMD ERR seq; an ETX inside it (a malformed or
     * truncated frame) would make this subtraction wrap. */
    if (end < 4)
        return -1;

    escaped_length = end - 4;
    decoded_length = agt_unescape(in + 4, escaped_length, decoded,
                                 sizeof(decoded));
    if (decoded_length < 2)
        return -1;
    decoded_length -= 2;

    checksum = (uint32_t)LE_AGT_CMD + LE_AGT_ERR;
    for (i = 0; i < (size_t)decoded_length; ++i)
        checksum += decoded[i];
    checksum &= 0xFFFFU;
    if (decoded[decoded_length] != (uint8_t)(checksum >> 8) ||
        decoded[decoded_length + 1] != (uint8_t)checksum)
        return -1;
    if ((size_t)decoded_length > payload_size)
        return -1;
    if (payload && decoded_length)
        memcpy(payload, decoded, (size_t)decoded_length);
    if (consumed)
        *consumed = end + 1;
    return decoded_length;
}

/* ---- directive builders ------------------------------------------------ */

/*
 * Wrap an already-serialized `Directive` body in an AGT Message{1: bytes}.
 * Returns the message length, or 0 on overflow.
 */
static size_t agt_wrap_message(uint8_t *out, size_t out_size,
                               const uint8_t *directive,
                               size_t directive_length)
{
    struct pb_writer w;

    if (!out || directive_length == 0)
        return 0;
    w.data = out;
    w.size = out_size;
    w.used = 0;
    w.overflow = 0;
    pb_message(&w, 1, directive, directive_length);
    return w.overflow ? 0 : w.used;
}

/* Header{1 namespace, 2 name}; the Echo omits the ids. */
static size_t agt_header(uint8_t *out, size_t out_size, const char *namespace,
                         const char *name)
{
    struct pb_writer w;

    w.data = out;
    w.size = out_size;
    w.used = 0;
    w.overflow = 0;
    pb_string(&w, 1, namespace);
    pb_string(&w, 2, name);
    return w.overflow ? 0 : w.used;
}

static size_t agt_directive(uint8_t *out, size_t out_size, const char *namespace,
                            const char *name, const uint8_t *payload,
                            size_t payload_length)
{
    uint8_t header[128];
    size_t header_length;
    struct pb_writer w;

    header_length = agt_header(header, sizeof(header), namespace, name);
    if (!header_length)
        return 0;
    w.data = out;
    w.size = out_size;
    w.used = 0;
    w.overflow = 0;
    pb_message(&w, 1, header, header_length);
    if (payload && payload_length)
        pb_message(&w, 2, payload, payload_length);
    return w.overflow ? 0 : w.used;
}

size_t le_agt_build_discover(uint8_t *out, size_t out_size)
{
    uint8_t directive[LE_AGT_PAYLOAD_MAX];
    size_t length;

    length = agt_directive(directive, sizeof(directive), "Alexa.Discovery",
                           "Discover", NULL, 0);
    return length ? agt_wrap_message(out, out_size, directive, length) : 0;
}

size_t le_agt_build_state_update(uint8_t *out, size_t out_size,
                                 const char *name, const char *value)
{
    uint8_t states[4][96];
    size_t state_lengths[4];
    uint8_t payload[LE_AGT_PAYLOAD_MAX];
    uint8_t directive[LE_AGT_PAYLOAD_MAX];
    size_t payload_length = 0;
    size_t directive_length;
    struct pb_writer w;
    size_t i;

    if (!name || !value)
        return 0;
    /*
     * states[] is a repeated message of {1 name, 2 value}; a single state
     * is the common case, and the same writer covers more later.
     */
    w.data = states[0];
    w.size = sizeof(states[0]);
    w.used = 0;
    w.overflow = 0;
    pb_string(&w, 1, name);
    pb_string(&w, 2, value);
    if (w.overflow)
        return 0;
    state_lengths[0] = w.used;

    w.data = payload;
    w.size = sizeof(payload);
    w.used = 0;
    w.overflow = 0;
    for (i = 0; i < 1; ++i)
        pb_message(&w, 1, states[i], state_lengths[i]);
    if (w.overflow)
        return 0;
    payload_length = w.used;

    directive_length = agt_directive(directive, sizeof(directive),
                                     "Alexa.Gadget.StateListener",
                                     "StateUpdate", payload, payload_length);
    return directive_length
        ? agt_wrap_message(out, out_size, directive, directive_length) : 0;
}

size_t le_agt_build_clear_indicator(uint8_t *out, size_t out_size)
{
    uint8_t directive[LE_AGT_PAYLOAD_MAX];
    size_t length;

    length = agt_directive(directive, sizeof(directive), "Notifications",
                           "ClearIndicator", NULL, 0);
    return length ? agt_wrap_message(out, out_size, directive, length) : 0;
}

size_t le_agt_build_set_indicator(uint8_t *out, size_t out_size)
{
    uint8_t payload[16];
    uint8_t directive[LE_AGT_PAYLOAD_MAX];
    size_t payload_length;
    size_t directive_length;
    struct pb_writer w;

    w.data = payload;
    w.size = sizeof(payload);
    w.used = 0;
    w.overflow = 0;
    pb_tag(&w, 1, 0); /* persistVisualIndicator = true */
    pb_varint(&w, 1);
    if (w.overflow)
        return 0;
    payload_length = w.used;

    directive_length = agt_directive(directive, sizeof(directive),
                                     "Notifications", "SetIndicator",
                                     payload, payload_length);
    return directive_length
        ? agt_wrap_message(out, out_size, directive, directive_length) : 0;
}

size_t le_agt_build_set_alert(uint8_t *out, size_t out_size, const char *token,
                              const char *alert_type,
                              const char *scheduled_time)
{
    uint8_t payload[LE_AGT_PAYLOAD_MAX];
    uint8_t directive[LE_AGT_PAYLOAD_MAX];
    size_t payload_length;
    size_t directive_length;
    struct pb_writer w;

    if (!token || !alert_type || !scheduled_time)
        return 0;
    w.data = payload;
    w.size = sizeof(payload);
    w.used = 0;
    w.overflow = 0;
    pb_string(&w, 1, token);
    pb_string(&w, 2, alert_type);
    pb_string(&w, 3, scheduled_time);
    if (w.overflow)
        return 0;
    payload_length = w.used;

    directive_length = agt_directive(directive, sizeof(directive), "Alerts",
                                     "SetAlert", payload, payload_length);
    return directive_length
        ? agt_wrap_message(out, out_size, directive, directive_length) : 0;
}

size_t le_agt_build_delete_alert(uint8_t *out, size_t out_size,
                                 const char *token)
{
    uint8_t payload[128];
    uint8_t directive[LE_AGT_PAYLOAD_MAX];
    size_t payload_length;
    size_t directive_length;
    struct pb_writer w;

    if (!token)
        return 0;
    w.data = payload;
    w.size = sizeof(payload);
    w.used = 0;
    w.overflow = 0;
    pb_string(&w, 1, token);
    if (w.overflow)
        return 0;
    payload_length = w.used;

    directive_length = agt_directive(directive, sizeof(directive), "Alerts",
                                     "DeleteAlert", payload, payload_length);
    return directive_length
        ? agt_wrap_message(out, out_size, directive, directive_length) : 0;
}

/* Copy a length-delimited header string out of a parsed field. */
static void copy_field(const uint8_t *data, size_t length, char *out,
                       size_t out_size)
{
    size_t copy;

    if (!out || out_size == 0)
        return;
    copy = length < out_size - 1 ? length : out_size - 1;
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
    int seen_namespace = 0;
    int seen_name = 0;

    if (!payload || payload_length == 0)
        return -1;
    outer.data = payload;
    outer.length = payload_length;
    outer.offset = 0;
    if (pb_read_varint(&outer, &(uint32_t){0}) != 0)
        return -1;
    /*
     * Message{1: bytes}: skip the tag, read the nested Event.
     */
    outer.offset = 0;
    {
        uint32_t tag;
        if (pb_read_varint(&outer, &tag) != 0 || (tag >> 3) != 1 ||
            (tag & 7U) != 2U)
            return -1;
    }
    if (pb_read_bytes(&outer, &message, &message_length) != 0)
        return -1;

    /* Event{1: Header}. */
    event.data = message;
    event.length = message_length;
    event.offset = 0;
    {
        uint32_t tag;
        if (pb_read_varint(&event, &tag) != 0 || (tag >> 3) != 1 ||
            (tag & 7U) != 2U)
            return -1;
    }
    if (pb_read_bytes(&event, &event_body, &event_length) != 0)
        return -1;

    header.data = event_body;
    header.length = event_length;
    header.offset = 0;
    while (header.offset < header.length) {
        uint32_t tag;
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
            copy_field(value, value_length, namespace_out, namespace_size);
            seen_namespace = 1;
        } else if ((tag >> 3) == 2U) {
            copy_field(value, value_length, name_out, name_size);
            seen_name = 1;
        }
    }
    return seen_namespace && seen_name ? 0 : -1;
}
