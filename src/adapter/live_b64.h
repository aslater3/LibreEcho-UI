#ifndef LIBREECHO_LIVE_B64_H
#define LIBREECHO_LIVE_B64_H

/*
 * Base64 for the GPT-Live WebSocket transport.
 *
 * The protocol carries audio as base64 inside JSON text frames, so this is on
 * the hot path in both directions: every 10 ms of microphone audio is encoded,
 * and every model audio delta is decoded.
 *
 * Bounded by construction. There is no allocation here and no growing buffer:
 * the caller passes the destination and its size, and a result that does not
 * fit is an error rather than a truncation. A silently truncated audio frame
 * would be delivered to the model as a shorter utterance and to the speaker as
 * a glitch, and neither is diagnosable after the fact.
 */

#include <stddef.h>

/* Encoded length of `count` bytes, excluding the terminating NUL. */
size_t le_b64_encoded_size(size_t count);

/*
 * Encode `count` bytes into `out` as NUL-terminated base64.
 * `out_size` must be at least le_b64_encoded_size(count) + 1.
 * Returns the number of characters written, or 0 if it does not fit.
 */
size_t le_b64_encode(const void *input, size_t count, char *out,
                     size_t out_size);

/*
 * Decode NUL-terminated base64 into `out`.
 * Returns the number of bytes written, or 0 on malformed input or if the
 * result does not fit. Accepts both padded and unpadded input, and ignores
 * ASCII whitespace, because a JSON encoder on the server side is free to wrap
 * a long value.
 */
size_t le_b64_decode(const char *input, void *out, size_t out_size);

#endif