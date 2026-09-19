#ifndef LIBREECHO_WS_CLIENT_H
#define LIBREECHO_WS_CLIENT_H

/*
 * Minimal RFC 6455 WebSocket client.
 *
 * Deliberately not tied to a socket or to TLS. The byte stream is supplied as
 * three function pointers, so the same code runs over the device's mbedTLS
 * session, over a plain socket in a host test, and over a socketpair in a unit
 * test - and the framing, which is the part that is easy to get wrong, is
 * exercised identically in all three.
 *
 * What is supported is what the GPT-Live protocol needs: one unfragmented text
 * message per frame, ping/pong, and close. Fragmentation, extensions and
 * compression are refused rather than half-implemented; a server that needs
 * them will say so and this fails closed.
 *
 * Client-to-server frames are masked and server-to-client frames must not be.
 * The mask key comes from /dev/urandom and the connection is refused if it
 * cannot be read: RFC 6455 requires the key to be unpredictable, and a
 * predictable one defeats the reason masking exists.
 *
 * Bounded: fixed receive buffers, no allocation, no growth.
 */

#include <stddef.h>
#include <stdint.h>

/* Largest single frame payload accepted. The protocol's biggest message is a
   base64 audio delta; 64 KiB holds 48 KiB of PCM, well past one chunk. */
#define LE_WS_MAX_PAYLOAD (64U * 1024U)
#define LE_WS_PATH_MAX 256

/*
 * A byte stream. `recv` returns bytes read, 0 on clean end of stream, -1 on
 * error, and -2 when nothing is available yet (the caller retries until its
 * own deadline, so a non-blocking socket is fine).
 */
struct le_ws_stream {
    void *context;
    long (*recv)(void *context, void *buffer, size_t length);
    long (*send)(void *context, const void *buffer, size_t length);
};

struct le_ws {
    const struct le_ws_stream *stream;
    int urandom;
    int connected;
    int close_received;
    /* Longest a single send may spend waiting for the stream to accept bytes. */
    int send_timeout_ms;
    /* Frame state survives EAGAIN at any byte boundary. */
    unsigned char rx_header[10];
    size_t rx_header_used, rx_header_need, rx_used, rx_length;
    unsigned int rx_opcode;
    unsigned char rx_payload[LE_WS_MAX_PAYLOAD];
    /* Complete masked frames queue atomically. TLS retries use an immutable
       chunk, never a caller-owned or moving buffer. */
    unsigned char tx_queue[(LE_WS_MAX_PAYLOAD + 14U) * 2U];
    size_t tx_used;
    unsigned char tx_chunk[4096];
    size_t tx_chunk_used, tx_chunk_sent;
    uint64_t tx_stalled_ms;
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t bytes_in;
    uint64_t bytes_out;
};

/*
 * Perform the HTTP Upgrade handshake against `host` (used for Host: and, by
 * the caller, for TLS SNI) and `path`. `extra_headers` is appended verbatim,
 * one CRLF-terminated line per header, or NULL.
 *
 * Returns 0 on success. On failure writes one bounded sentence into `detail`.
 * The response is validated: 101, upgrade to websocket, and a
 * Sec-WebSocket-Accept matching the key, so a proxy or a captive portal that
 * answers 200 is rejected instead of being treated as a session.
 */
int le_ws_connect(struct le_ws *ws, const struct le_ws_stream *stream,
                  const char *host, const char *path,
                  const char *extra_headers, int timeout_ms,
                  char *detail, size_t detail_size);

/* Nonblocking queue/pump. Queue exhaustion fails rather than losing bytes. */
int le_ws_pump(struct le_ws *ws);
size_t le_ws_send_capacity(const struct le_ws *ws);
/* Send one unfragmented text message. Returns 0 when accepted to the queue. */
int le_ws_send_text(struct le_ws *ws, const char *payload, size_t length);

/* Send a close frame. Best effort; the caller closes the stream after. */
int le_ws_send_close(struct le_ws *ws, int code);

/*
 * Read one text message into `out` (NUL-terminated).
 * Returns 1 for a message, 2 when the peer closed, 0 when `timeout_ms`
 * elapsed with nothing to report, and -1 on error or protocol violation.
 * Ping frames are answered transparently and do not surface here.
 */
int le_ws_read_text(struct le_ws *ws, char *out, size_t out_size,
                    int timeout_ms);

void le_ws_close(struct le_ws *ws);

#endif