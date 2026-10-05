/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <string.h>

#include "src/util/prte_hmac.h"

/* FIPS 180-4, section 4.2.2: the first 32 bits of the fractional parts of the
 * cube roots of the first 64 primes */
static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

/* Every word is read and written big-endian, a byte at a time, so the result
 * does not depend on the host's byte order or on the alignment of the input. */
static uint32_t load_be32(const uint8_t *p)
{
    return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8)
           | (uint32_t) p[3];
}

static void store_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static void sha256_compress(prte_sha256_ctx_t *ctx, const uint8_t *blk)
{
    uint32_t w[64], a, b, c, d, e, f, g, h, s0, s1, t1, t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = load_be32(blk + 4 * i);
    }
    for (i = 16; i < 64; i++) {
        s0 = ROTR(w[i - 15], 7) ^ ROTR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        s1 = ROTR(w[i - 2], 17) ^ ROTR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    a = ctx->h[0];
    b = ctx->h[1];
    c = ctx->h[2];
    d = ctx->h[3];
    e = ctx->h[4];
    f = ctx->h[5];
    g = ctx->h[6];
    h = ctx->h[7];

    for (i = 0; i < 64; i++) {
        s1 = ROTR(e, 6) ^ ROTR(e, 11) ^ ROTR(e, 25);
        t1 = h + s1 + ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
        s0 = ROTR(a, 2) ^ ROTR(a, 13) ^ ROTR(a, 22);
        t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
    ctx->h[5] += f;
    ctx->h[6] += g;
    ctx->h[7] += h;
}

void prte_sha256_init(prte_sha256_ctx_t *ctx)
{
    /* FIPS 180-4, section 5.3.3 */
    ctx->h[0] = 0x6a09e667;
    ctx->h[1] = 0xbb67ae85;
    ctx->h[2] = 0x3c6ef372;
    ctx->h[3] = 0xa54ff53a;
    ctx->h[4] = 0x510e527f;
    ctx->h[5] = 0x9b05688c;
    ctx->h[6] = 0x1f83d9ab;
    ctx->h[7] = 0x5be0cd19;
    ctx->nbits = 0;
    ctx->used = 0;
}

void prte_sha256_update(prte_sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *) data;
    size_t take;

    ctx->nbits += (uint64_t) len * 8;
    while (0 < len) {
        take = PRTE_SHA256_BLOCK_LEN - ctx->used;
        if (take > len) {
            take = len;
        }
        memcpy(ctx->block + ctx->used, p, take);
        ctx->used += take;
        p += take;
        len -= take;
        if (PRTE_SHA256_BLOCK_LEN == ctx->used) {
            sha256_compress(ctx, ctx->block);
            ctx->used = 0;
        }
    }
}

void prte_sha256_final(prte_sha256_ctx_t *ctx, uint8_t out[PRTE_SHA256_DIGEST_LEN])
{
    uint64_t nbits = ctx->nbits;
    int i;

    /* a single 1 bit, zeros up to 56 bytes into a block, then the message
     * length in bits as a big-endian 64-bit value */
    ctx->block[ctx->used++] = 0x80;
    if (PRTE_SHA256_BLOCK_LEN - 8 < ctx->used) {
        memset(ctx->block + ctx->used, 0, PRTE_SHA256_BLOCK_LEN - ctx->used);
        sha256_compress(ctx, ctx->block);
        ctx->used = 0;
    }
    memset(ctx->block + ctx->used, 0, PRTE_SHA256_BLOCK_LEN - 8 - ctx->used);
    for (i = 0; i < 8; i++) {
        ctx->block[PRTE_SHA256_BLOCK_LEN - 1 - i] = (uint8_t) (nbits >> (8 * i));
    }
    sha256_compress(ctx, ctx->block);

    for (i = 0; i < 8; i++) {
        store_be32(out + 4 * i, ctx->h[i]);
    }
    prte_secure_zero(ctx, sizeof(*ctx));
}

void prte_hmac_sha256(const uint8_t *key, size_t keylen, const void *msg, size_t len,
                      uint8_t out[PRTE_SHA256_DIGEST_LEN])
{
    prte_sha256_ctx_t ctx;
    uint8_t k[PRTE_SHA256_BLOCK_LEN], pad[PRTE_SHA256_BLOCK_LEN];
    uint8_t inner[PRTE_SHA256_DIGEST_LEN];
    size_t i;

    memset(k, 0, sizeof(k));
    if (PRTE_SHA256_BLOCK_LEN < keylen) {
        prte_sha256_init(&ctx);
        prte_sha256_update(&ctx, key, keylen);
        prte_sha256_final(&ctx, k);
    } else if (0 < keylen) {
        memcpy(k, key, keylen);
    }

    /* H((K ^ ipad) || msg) */
    for (i = 0; i < PRTE_SHA256_BLOCK_LEN; i++) {
        pad[i] = k[i] ^ 0x36;
    }
    prte_sha256_init(&ctx);
    prte_sha256_update(&ctx, pad, sizeof(pad));
    prte_sha256_update(&ctx, msg, len);
    prte_sha256_final(&ctx, inner);

    /* H((K ^ opad) || inner) */
    for (i = 0; i < PRTE_SHA256_BLOCK_LEN; i++) {
        pad[i] = k[i] ^ 0x5c;
    }
    prte_sha256_init(&ctx);
    prte_sha256_update(&ctx, pad, sizeof(pad));
    prte_sha256_update(&ctx, inner, sizeof(inner));
    prte_sha256_final(&ctx, out);

    prte_secure_zero(k, sizeof(k));
    prte_secure_zero(pad, sizeof(pad));
    prte_secure_zero(inner, sizeof(inner));
}

bool prte_secure_equal(const void *a, const void *b, size_t len)
{
    const volatile uint8_t *x = (const volatile uint8_t *) a;
    const volatile uint8_t *y = (const volatile uint8_t *) b;
    uint8_t diff = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        diff |= (uint8_t) (x[i] ^ y[i]);
    }
    return (0 == diff);
}

void prte_secure_zero(void *p, size_t len)
{
    volatile uint8_t *v = (volatile uint8_t *) p;

    while (0 < len--) {
        *v++ = 0;
    }
}
