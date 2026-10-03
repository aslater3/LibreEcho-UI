#define _POSIX_C_SOURCE 200809L

#include "live_b64.h"

#include <string.h>

static const char alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int value_of(unsigned char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+' || c == '-')
        return 62;
    if (c == '/' || c == '_')
        return 63;
    return -1;
}

size_t le_b64_encoded_size(size_t count)
{
    return ((count + 2U) / 3U) * 4U;
}

size_t le_b64_encode(const void *input, size_t count, char *out,
                     size_t out_size)
{
    const unsigned char *p = input;
    size_t needed = le_b64_encoded_size(count);
    size_t in = 0;
    size_t used = 0;

    if (!out || !input || out_size < needed + 1U)
        return 0;
    while (in + 3U <= count) {
        unsigned int group = ((unsigned int)p[in] << 16) |
                             ((unsigned int)p[in + 1] << 8) |
                             (unsigned int)p[in + 2];

        out[used++] = alphabet[(group >> 18) & 0x3fU];
        out[used++] = alphabet[(group >> 12) & 0x3fU];
        out[used++] = alphabet[(group >> 6) & 0x3fU];
        out[used++] = alphabet[group & 0x3fU];
        in += 3U;
    }
    if (in < count) {
        unsigned int group = (unsigned int)p[in] << 16;
        int remaining = (int)(count - in);

        if (remaining == 2)
            group |= (unsigned int)p[in + 1] << 8;
        out[used++] = alphabet[(group >> 18) & 0x3fU];
        out[used++] = alphabet[(group >> 12) & 0x3fU];
        out[used++] = remaining == 2 ? alphabet[(group >> 6) & 0x3fU] : '=';
        out[used++] = '=';
    }
    out[used] = '\0';
    return used;
}

size_t le_b64_decode(const char *input, void *out, size_t out_size)
{
    unsigned char *p = out;
    unsigned int accumulator = 0;
    unsigned int bits = 0;
    size_t used = 0;

    if (!input || !out)
        return 0;
    for (; *input; ++input) {
        unsigned char c = (unsigned char)*input;
        int value;

        /* Padding ends the payload; whitespace is tolerated. */
        if (c == '=')
            break;
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;
        value = value_of(c);
        if (value < 0)
            return 0;
        accumulator = (accumulator << 6) | (unsigned int)value;
        bits += 6U;
        if (bits >= 8U) {
            bits -= 8U;
            if (used >= out_size)
                return 0;
            p[used++] = (unsigned char)((accumulator >> bits) & 0xffU);
        }
    }
    /*
     * A trailing group of six bits is padding, not data; anything else is a
     * malformed length and is refused rather than guessed at.
     */
    if (bits >= 6U)
        return 0;
    return used;
}