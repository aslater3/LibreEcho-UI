/*
 * Alexa Gadget Toolkit (AGT) codec for the SPP transport.
 *
 * An Echo Wall Clock is an Alexa Gadget: it speaks Amazon's documented AGT
 * framing over SPP/RFCOMM channel 4 and protobuf-encoded messages inside it.
 * This module is transport-independent: it frames and parses bytes, and
 * builds the directives a real Echo sends.  The caller owns the socket.
 *
 * Frame:  F0 | cmd=02 | err=00 | seq | escaped(payload + checksum16_be) | F1
 *         checksum = (cmd + err + sum(payload)) & 0xFFFF
 *         F0/F1/F2 inside payload/checksum are sent as F2, (byte ^ F2)
 *
 * Payload: Message{1: bytes} wrapping
 *   Directive{1: Header{1 namespace, 2 name, 3 messageId, 4 dialogRequestId},
 *             2: payload}                                        (partner -> gadget)
 *   Event    {1: Header{1 namespace, 2 name, 3 messageId},
 *             2: payload}                                        (gadget -> partner)
 *
 * The clock is strict and silent: it ignores messages carrying fields a real
 * Echo omits, so the builders emit the minimum field set (no messageId, no
 * dialogRequestId, no empty payload).
 */
#ifndef LIBREECHO_BT_AGT_H
#define LIBREECHO_BT_AGT_H

#include <stddef.h>
#include <stdint.h>

#define LE_AGT_STX 0xF0
#define LE_AGT_ETX 0xF1
#define LE_AGT_ESC 0xF2

#define LE_AGT_CMD 0x02
#define LE_AGT_ERR 0x00

/* Largest payload this codec can build or parse inside one frame. */
#define LE_AGT_PAYLOAD_MAX 512

/* Encoding scratch: escaped payload plus checksum never exceeds this. */
#define LE_AGT_FRAME_MAX (LE_AGT_PAYLOAD_MAX * 2 + 8)

/*
 * Encode one frame around `payload`.  `seq` is the frame sequence number and
 * must not be one of the reserved framing bytes 0xF0..0xF2; the AGT caller
 * skips them (`bt_profile.c`: `if (++seq > 0xEF) seq = 1`), and
 * `le_agt_encode` also rejects them so the encoder can never produce a frame
 * `le_agt_decode` rejects.  Returns the frame length, or 0 when `seq` is
 * reserved, the payload is too large, or the output buffer is short.
 */
size_t le_agt_encode(uint8_t *out, size_t out_size, uint8_t seq,
                     const uint8_t *payload, size_t payload_length);

/*
 * Decode one frame from `in`.  On success `*consumed` is the number of bytes
 * the frame occupied (so a caller can advance a stream buffer) and the
 * payload is written to `payload`.  Returns the payload length, or -1 on a
 * malformed or truncated frame.
 *
 * Strict about what it accepts, because the peer is not trusted: a payload
 * longer than LE_AGT_PAYLOAD_MAX, a reserved byte in `seq`, an unescaped
 * reserved byte in the body, and an escape that decodes to a non-reserved
 * byte are all rejected.  That is exactly the set `le_agt_encode` can
 * produce.
 */
int le_agt_decode(const uint8_t *in, size_t in_length, uint8_t *payload,
                  size_t payload_size, size_t *consumed);

/* ---- directive builders (partner -> gadget) ---------------------------- */

/*
 * Alexa.Discovery / Discover.  Byte-identical to what a real Echo sends:
 * header only, no messageId, no payload.
 */
size_t le_agt_build_discover(uint8_t *out, size_t out_size);

/* Alexa.Gadget.StateListener / StateUpdate with one `name = value` state. */
size_t le_agt_build_state_update(uint8_t *out, size_t out_size,
                                 const char *name, const char *value);

/* Notifications / ClearIndicator. */
size_t le_agt_build_clear_indicator(uint8_t *out, size_t out_size);

/* Notifications / SetIndicator with persistVisualIndicator = true. */
size_t le_agt_build_set_indicator(uint8_t *out, size_t out_size);

/*
 * Alerts / SetAlert.  `alert_type` is "TIMER", "ALARM" or "REMINDER";
 * `scheduled_time` is the alert's END time as local ISO-8601, e.g.
 * "2026-10-14T10:44:19-04:00".  `token` is any unique decimal string.
 */
size_t le_agt_build_set_alert(uint8_t *out, size_t out_size, const char *token,
                              const char *alert_type,
                              const char *scheduled_time);

/* Alerts / DeleteAlert for `token`. */
size_t le_agt_build_delete_alert(uint8_t *out, size_t out_size,
                                 const char *token);

/*
 * Read the namespace and name out of a gadget Event payload (the decoded
 * frame payload).  Either output may be NULL.  Returns 0 when the payload
 * carries a header, -1 otherwise.
 */
int le_agt_read_event_header(const uint8_t *payload, size_t payload_length,
                             char *namespace_out, size_t namespace_size,
                             char *name_out, size_t name_size);

#endif /* LIBREECHO_BT_AGT_H */
