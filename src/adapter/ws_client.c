#define _POSIX_C_SOURCE 200809L

#include "ws_client.h"
#include "live_b64.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define OPCODE_TEXT 0x1
#define OPCODE_BINARY 0x2
#define OPCODE_CLOSE 0x8
#define OPCODE_PING 0x9
#define OPCODE_PONG 0xA

/* Read outcomes that are not "bytes". */
#define READ_ERROR (-1)
#define READ_TIMEOUT (-2)
#define READ_CLOSED (-3)

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000ULL + (uint64_t)(now.tv_nsec / 1000000L);
}

static void say(char *detail, size_t size, const char *message)
{
    if (detail && size)
        snprintf(detail, size, "%s", message);
}

/* --- SHA-1, for the handshake accept check ------------------------------ */

struct sha1 {
    uint32_t h[5];
    uint64_t length;
    unsigned char block[64];
    size_t used;
};

static uint32_t rol(uint32_t value, int bits)
{
    return (value << bits) | (value >> (32 - bits));
}

static void sha1_init(struct sha1 *s)
{
    s->h[0] = 0x67452301U;
    s->h[1] = 0xEFCDAB89U;
    s->h[2] = 0x98BADCFEU;
    s->h[3] = 0x10325476U;
    s->h[4] = 0xC3D2E1F0U;
    s->length = 0;
    s->used = 0;
}

static void sha1_block(struct sha1 *s, const unsigned char *block)
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    int i;

    for (i = 0; i < 16; ++i)
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    for (i = 16; i < 80; ++i)
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0];
    b = s->h[1];
    c = s->h[2];
    d = s->h[3];
    e = s->h[4];
    for (i = 0; i < 80; ++i) {
        uint32_t f;
        uint32_t k;
        uint32_t temp;

        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999U;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }
        temp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }
    s->h[0] += a;
    s->h[1] += b;
    s->h[2] += c;
    s->h[3] += d;
    s->h[4] += e;
}

static void sha1_update(struct sha1 *s, const void *data, size_t length)
{
    const unsigned char *p = data;

    s->length += length;
    while (length) {
        size_t take = 64U - s->used;

        if (take > length)
            take = length;
        memcpy(s->block + s->used, p, take);
        s->used += take;
        p += take;
        length -= take;
        if (s->used == 64U) {
            sha1_block(s, s->block);
            s->used = 0;
        }
    }
}

static void sha1_final(struct sha1 *s, unsigned char out[20])
{
    uint64_t bits = s->length * 8ULL;
    unsigned char pad = 0x80;
    unsigned char zero = 0;
    unsigned char length_bytes[8];
    int i;

    sha1_update(s, &pad, 1);
    while (s->used != 56U)
        sha1_update(s, &zero, 1);
    for (i = 0; i < 8; ++i)
        length_bytes[i] = (unsigned char)(bits >> (56 - i * 8));
    sha1_update(s, length_bytes, sizeof(length_bytes));
    for (i = 0; i < 5; ++i) {
        out[i * 4] = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)s->h[i];
    }
}

/* --- stream helpers ----------------------------------------------------- */

static long read_exact(struct le_ws *ws, unsigned char *buffer, size_t length,
                       uint64_t deadline)
{
    size_t used = 0;

    while (used < length) {
        long got = ws->stream->recv(ws->stream->context, buffer + used,
                                    length - used);

        if (got > 0) {
            used += (size_t)got;
            continue;
        }
        if (got == 0)
            return READ_CLOSED;
        if (got != -2)
            return READ_ERROR;
        if (monotonic_ms() >= deadline)
            return READ_TIMEOUT;
    }
    return (long)used;
}

static long write_all(struct le_ws *ws, const void *buffer, size_t length)
{
    const unsigned char *p = buffer;
    size_t sent = 0;
    uint64_t deadline = monotonic_ms() +
        (uint64_t)(ws->send_timeout_ms > 0 ? ws->send_timeout_ms : 5000);

    while (sent < length) {
        long wrote = ws->stream->send(ws->stream->context, p + sent,
                                      length - sent);

        if (wrote > 0) {
            sent += (size_t)wrote;
            continue;
        }
        if (wrote == 0)
            return -1;
        /*
         * The stream is non-blocking, so -2 means "not right now". A peer that
         * stops reading must not be able to hold the daemon's poll loop open
         * indefinitely, so this is bounded like every other wait here.
         */
        if (monotonic_ms() >= deadline)
            return -1;
        if (wrote != -2)
            return -1;
    }
    return 0;
}

static int random_bytes(struct le_ws *ws, unsigned char *out, size_t length)
{
    size_t used = 0;

    if (ws->urandom < 0)
        return -1;
    while (used < length) {
        ssize_t got = read(ws->urandom, out + used, length - used);

        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        used += (size_t)got;
    }
    return 0;
}

/* --- handshake ---------------------------------------------------------- */

static int accept_matches(const char *response, const char *key)
{
    struct sha1 hash;
    unsigned char digest[20];
    char expected[64];
    char combined[128];
    const char *header;
    int length;

    if (snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID) >=
        (int)sizeof(combined))
        return 0;
    sha1_init(&hash);
    sha1_update(&hash, combined, strlen(combined));
    sha1_final(&hash, digest);
    if (le_b64_encode(digest, sizeof(digest), expected, sizeof(expected)) == 0)
        return 0;
    header = response;
    while ((header = strstr(header, "\r\n")) != NULL) {
        const char *name;
        const char *colon;
        size_t name_length;

        header += 2;
        name = header;
        colon = strchr(name, ':');
        if (!colon)
            break;
        name_length = (size_t)(colon - name);
        if (name_length == strlen("Sec-WebSocket-Accept") &&
            !strncasecmp(name, "Sec-WebSocket-Accept", name_length)) {
            header = colon + 1;
            while (*header == ' ' || *header == '\t')
                ++header;
            length = (int)strlen(expected);
            return !strncmp(header, expected, (size_t)length) &&
                   (header[length] == '\r' || header[length] == '\n');
        }
    }
    return 0;
}

static int response_header_has_token(const char *response,
                                     const char *header_name,
                                     const char *token)
{
    const char *line = response;
    size_t name_length = strlen(header_name);

    while ((line = strstr(line, "\r\n")) != NULL) {
        const char *colon;
        const char *value;
        const char *end;

        line += 2;
        colon = strchr(line, ':');
        if (!colon)
            break;
        if ((size_t)(colon - line) != name_length ||
            strncasecmp(line, header_name, name_length))
            continue;
        value = colon + 1;
        end = strstr(value, "\r\n");
        if (!end)
            return 0;
        while (value < end) {
            const char *item_end = value;

            while (value < end && (*value == ' ' || *value == '\t' ||
                                   *value == ','))
                ++value;
            item_end = value;
            while (item_end < end && *item_end != ',' && *item_end != ' ' &&
                   *item_end != '\t')
                ++item_end;
            if ((size_t)(item_end - value) == strlen(token) &&
                !strncasecmp(value, token, strlen(token)))
                return 1;
            value = item_end;
        }
        return 0;
    }
    return 0;
}

int le_ws_connect(struct le_ws *ws, const struct le_ws_stream *stream,
                  const char *host, const char *path,
                  const char *extra_headers, int timeout_ms,
                  char *detail, size_t detail_size)
{
    unsigned char nonce[16];
    char key[32];
    char request[1024];
    char response[2048];
    uint64_t deadline;
    size_t used = 0;
    int length;

    if (detail && detail_size)
        detail[0] = '\0';
    if (!ws || !stream || !stream->recv || !stream->send || !host || !path)
        return -1;
    memset(ws, 0, sizeof(*ws));
    ws->stream = stream;
    ws->urandom = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (ws->urandom < 0) {
        say(detail, detail_size,
            "GPT-Live unavailable: no source of random for the socket "
            "handshake.");
        return -1;
    }
    if (random_bytes(ws, nonce, sizeof(nonce)) < 0 ||
        le_b64_encode(nonce, sizeof(nonce), key, sizeof(key)) == 0) {
        say(detail, detail_size,
            "GPT-Live unavailable: no source of random for the socket "
            "handshake.");
        close(ws->urandom);
        ws->urandom = -1;
        return -1;
    }
    length = snprintf(request, sizeof(request),
                      "GET %s HTTP/1.1\r\n"
                      "Host: %s\r\n"
                      "Upgrade: websocket\r\n"
                      "Connection: Upgrade\r\n"
                      "Sec-WebSocket-Key: %s\r\n"
                      "Sec-WebSocket-Version: 13\r\n"
                      "%s\r\n",
                      path, host, key, extra_headers ? extra_headers : "");
    if (length <= 0 || (size_t)length >= sizeof(request)) {
        say(detail, detail_size, "GPT-Live unavailable: handshake too large.");
        le_ws_close(ws);
        return -1;
    }
    deadline = monotonic_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 8000);
    if (write_all(ws, request, (size_t)length) < 0) {
        say(detail, detail_size,
            "GPT-Live unavailable: could not send the handshake.");
        le_ws_close(ws);
        return -1;
    }
    /* Read headers one byte at a time until the blank line: over-reading would
       swallow the start of the first frame. */
    for (;;) {
        long got;

        if (used + 1U >= sizeof(response)) {
            say(detail, detail_size,
                "GPT-Live unavailable: handshake response too large.");
            le_ws_close(ws);
            return -1;
        }
        got = read_exact(ws, (unsigned char *)response + used, 1, deadline);
        if (got == READ_TIMEOUT) {
            say(detail, detail_size,
                "GPT-Live unavailable: the server did not answer in time.");
            le_ws_close(ws);
            return -1;
        }
        if (got < 0) {
            say(detail, detail_size,
                "GPT-Live unavailable: the connection closed during the "
                "handshake.");
            le_ws_close(ws);
            return -1;
        }
        if (used >= 3U && !memcmp(response + used - 3, "\r\n\r\n", 4))
            break;
        ++used;
    }
    response[used + 1] = '\0';
    if (strncmp(response, "HTTP/1.1 101 ", 13) &&
        strncmp(response, "HTTP/1.0 101 ", 13)) {
        say(detail, detail_size,
            "GPT-Live unavailable: the server refused the WebSocket upgrade.");
        le_ws_close(ws);
        return -1;
    }
    if (!response_header_has_token(response, "Upgrade", "websocket") ||
        !response_header_has_token(response, "Connection", "upgrade") ||
        !accept_matches(response, key)) {
        say(detail, detail_size,
            "GPT-Live unavailable: the WebSocket handshake did not verify.");
        le_ws_close(ws);
        return -1;
    }
    ws->connected = 1;
    return 0;
}

/* --- frames ------------------------------------------------------------- */

static int send_frame(struct le_ws *ws, int opcode, const void *payload,
                      size_t length)
{
    unsigned char header[14];
    unsigned char mask[4];
    /* Masking scratch. The caller's buffer is const and is not the transport's
       to scribble on; a masked copy also never needs the whole frame resident. */
    unsigned char scratch[512];
    const unsigned char *body = payload;
    size_t header_length;
    size_t offset;
    size_t i;

    if (!ws->connected || length > LE_WS_MAX_PAYLOAD)
        return -1;
    if (random_bytes(ws, mask, sizeof(mask)) < 0)
        return -1;
    header[0] = (unsigned char)(0x80 | opcode);
    if (length < 126U) {
        header[1] = (unsigned char)(0x80U | length);
        header_length = 2;
    } else if (length <= 0xffffU) {
        header[1] = 0x80U | 126U;
        header[2] = (unsigned char)(length >> 8);
        header[3] = (unsigned char)length;
        header_length = 4;
    } else {
        header[1] = 0x80U | 127U;
        for (i = 0; i < 8; ++i)
            header[2 + i] = (unsigned char)((uint64_t)length >>
                                            (56 - i * 8));
        header_length = 10;
    }
    memcpy(header + header_length, mask, sizeof(mask));
    header_length += sizeof(mask);
    if (write_all(ws, header, header_length) < 0)
        return -1;
    for (offset = 0; offset < length; offset += sizeof(scratch)) {
        size_t chunk = length - offset;

        if (chunk > sizeof(scratch))
            chunk = sizeof(scratch);
        for (i = 0; i < chunk; ++i)
            scratch[i] = body[offset + i] ^ mask[(offset + i) % 4U];
        if (write_all(ws, scratch, chunk) < 0)
            return -1;
    }
    ++ws->frames_out;
    ws->bytes_out += header_length + length;
    return 0;
}

int le_ws_send_text(struct le_ws *ws, const char *payload, size_t length)
{
    if (!payload)
        return -1;
    return send_frame(ws, OPCODE_TEXT, payload, length);
}

int le_ws_send_close(struct le_ws *ws, int code)
{
    unsigned char body[2];

    if (!ws->connected)
        return -1;
    body[0] = (unsigned char)(code >> 8);
    body[1] = (unsigned char)code;
    return send_frame(ws, OPCODE_CLOSE, body, sizeof(body));
}

/*
 * Read one frame into `payload`, reporting its opcode. Returns 0 on success
 * and a READ_* code otherwise. Server frames must be unmasked; a masked one is
 * a protocol violation and is refused.
 */
static int read_frame(struct le_ws *ws, int *opcode, unsigned char *payload,
                      size_t capacity, size_t *length, uint64_t deadline)
{
    unsigned char header[2];
    unsigned char extended[8];
    uint64_t payload_length;
    int fin;

    {
        long got = read_exact(ws, header, sizeof(header), deadline);

        /*
         * A quiet moment and a closed stream are different outcomes: reporting
         * a timeout as a close would end a healthy session every time the
         * model paused.
         */
        if (got == READ_TIMEOUT)
            return READ_TIMEOUT;
        if (got == READ_CLOSED)
            return READ_CLOSED;
        if (got < 0)
            return READ_ERROR;
    }
    fin = (header[0] & 0x80) != 0;
    *opcode = header[0] & 0x0f;
    if (header[0] & 0x70) {                  /* RSV: no extensions negotiated */
        return READ_ERROR;
    }
    if (header[1] & 0x80) {                  /* masked server frame */
        return READ_ERROR;
    }
    payload_length = header[1] & 0x7fU;
    if (payload_length == 126U) {
        int got = (int)read_exact(ws, extended, 2, deadline);
        if (got == READ_TIMEOUT)
            return READ_TIMEOUT;
        if (got == READ_CLOSED)
            return READ_CLOSED;
        if (got < 0)
            return READ_ERROR;
        payload_length = ((uint64_t)extended[0] << 8) | extended[1];
    } else if (payload_length == 127U) {
        int i;

        int got = (int)read_exact(ws, extended, 8, deadline);
        if (got == READ_TIMEOUT)
            return READ_TIMEOUT;
        if (got == READ_CLOSED)
            return READ_CLOSED;
        if (got < 0)
            return READ_ERROR;
        payload_length = 0;
        for (i = 0; i < 8; ++i)
            payload_length = (payload_length << 8) | extended[i];
    }
    if (payload_length > capacity)
        return READ_ERROR;                   /* refuse rather than truncate */
    if (payload_length) {
        int got = (int)read_exact(ws, payload, (size_t)payload_length,
                                  deadline);
        if (got == READ_TIMEOUT)
            return READ_TIMEOUT;
        if (got == READ_CLOSED)
            return READ_CLOSED;
        if (got < 0)
            return READ_ERROR;
    }
    if (*opcode >= OPCODE_CLOSE && (!fin || payload_length > 125U))
        return READ_ERROR;
    *length = (size_t)payload_length;
    ++ws->frames_in;
    ws->bytes_in += (uint64_t)payload_length;
    if (!fin && *opcode != OPCODE_CLOSE)
        return READ_ERROR;                   /* fragmentation unsupported */
    return 0;
}

int le_ws_read_text(struct le_ws *ws, char *out, size_t out_size,
                    int timeout_ms)
{
    static unsigned char payload[LE_WS_MAX_PAYLOAD];
    uint64_t deadline;

    if (!ws || !ws->connected || !out || out_size == 0)
        return -1;
    out[0] = '\0';
    deadline = monotonic_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 100);
    for (;;) {
        int opcode = 0;
        size_t length = 0;
        int result = read_frame(ws, &opcode, payload, sizeof(payload) - 1U,
                                &length, deadline);

        if (result == READ_TIMEOUT)
            return 0;
        if (result == READ_CLOSED) {
            ws->close_received = 1;
            return 2;
        }
        if (result < 0)
            return -1;
        if (opcode == OPCODE_PING) {
            if (send_frame(ws, OPCODE_PONG, payload, length) < 0)
                return -1;
            continue;
        }
        if (opcode == OPCODE_PONG)
            continue;
        if (opcode == OPCODE_CLOSE) {
            ws->close_received = 1;
            return 2;
        }
        if (opcode != OPCODE_TEXT)
            return -1;                       /* binary is not used here */
        if (length >= out_size)
            return -1;
        memcpy(out, payload, length);
        out[length] = '\0';
        return 1;
    }
}

void le_ws_close(struct le_ws *ws)
{
    if (!ws)
        return;
    ws->connected = 0;
    if (ws->urandom >= 0)
        close(ws->urandom);
    ws->urandom = -1;
}