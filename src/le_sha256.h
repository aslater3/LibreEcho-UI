/* Small self-contained SHA-256 shared by the provenance modules; keeps the UI
   target dependency-free. Static so each translation unit stays standalone. */
#ifndef LE_SHA256_H
#define LE_SHA256_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    unsigned int state[8];
    unsigned long long bits;
    unsigned char block[64];
    size_t used;
} le_sha256;

static unsigned int sha_rotr(unsigned int value, unsigned int count)
{
    return (value >> count) | (value << (32U - count));
}

static const unsigned int sha_k[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void sha256_transform(le_sha256 *ctx, const unsigned char *block)
{
    unsigned int w[64], a, b, c, d, e, f, g, h, t1, t2;
    unsigned int s0, s1, ch, maj;
    size_t i;
    for (i = 0; i < 16; ++i)
        w[i] = ((unsigned int)block[i * 4] << 24) |
               ((unsigned int)block[i * 4 + 1] << 16) |
               ((unsigned int)block[i * 4 + 2] << 8) |
               (unsigned int)block[i * 4 + 3];
    for (i = 16; i < 64; ++i) {
        s0 = sha_rotr(w[i - 15], 7) ^ sha_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        s1 = sha_rotr(w[i - 2], 17) ^ sha_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];
    for (i = 0; i < 64; ++i) {
        s1 = sha_rotr(e, 6) ^ sha_rotr(e, 11) ^ sha_rotr(e, 25);
        ch = (e & f) ^ (~e & g);
        t1 = h + s1 + ch + sha_k[i] + w[i];
        s0 = sha_rotr(a, 2) ^ sha_rotr(a, 13) ^ sha_rotr(a, 22);
        maj = (a & b) ^ (a & c) ^ (b & c);
        t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(le_sha256 *ctx)
{
    static const unsigned int initial[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };
    memcpy(ctx->state, initial, sizeof(initial));
    ctx->bits = 0;
    ctx->used = 0;
}

static void sha256_update(le_sha256 *ctx, const unsigned char *data, size_t size)
{
    while (size) {
        size_t take = sizeof(ctx->block) - ctx->used;
        if (take > size)
            take = size;
        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take;
        ctx->bits += (unsigned long long)take * 8ULL;
        data += take;
        size -= take;
        if (ctx->used == sizeof(ctx->block)) {
            sha256_transform(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

static void sha256_final(le_sha256 *ctx, char *out, size_t out_size)
{
    unsigned char length[8];
    size_t i;
    if (out_size < 65)
        return;
    for (i = 0; i < sizeof(length); ++i)
        length[7 - i] = (unsigned char)(ctx->bits >> (i * 8));
    sha256_update(ctx, (const unsigned char *)"\x80", 1);
    while (ctx->used != 56)
        sha256_update(ctx, (const unsigned char *)"\0", 1);
    sha256_update(ctx, length, sizeof(length));
    for (i = 0; i < 8; ++i)
        snprintf(out + i * 8, 9, "%08x", ctx->state[i]);
    out[64] = 0;
}

#endif
