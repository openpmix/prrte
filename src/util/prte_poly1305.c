/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Poly1305, RFC 8439 section 2.5.
 *
 * The accumulator h and the multiplier r are held as five 26-bit limbs, so
 * every partial product of a limb pair fits in 52 bits and a sum of five of
 * them in a uint64_t, with no 128-bit arithmetic.  Reduction modulo
 * p = 2^130 - 5 uses 2^130 = 5 (mod p): a limb product that lands above
 * 2^130 is folded back in multiplied by 5, which is why the r limbs that
 * meet h's upper limbs are pre-multiplied by 5 (s1..s4).
 *
 * Nothing here branches on, or indexes by, a value derived from the key or
 * the message, so the time taken depends on the length alone.
 */

#include "prte_config.h"

#include <string.h>

#include "src/util/prte_hmac.h"
#include "src/util/prte_poly1305.h"

#define LIMB_MASK 0x3ffffffu

/* little-endian, a byte at a time - independent of host order and alignment */
static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16)
           | ((uint32_t) p[3] << 24);
}

static void store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

void prte_poly1305_init(prte_poly1305_ctx_t *ctx, const uint8_t key[PRTE_POLY1305_KEY_LEN])
{
    /* r is the first half of the key, "clamped" (RFC 8439 2.5.1): the top
     * four bits of bytes 3, 7, 11 and 15 and the bottom two of bytes 4, 8
     * and 12 cleared.  The masks below do that while splitting it into
     * limbs. */
    ctx->r[0] = load_le32(key + 0) & 0x3ffffffu;
    ctx->r[1] = (load_le32(key + 3) >> 2) & 0x3ffff03u;
    ctx->r[2] = (load_le32(key + 6) >> 4) & 0x3ffc0ffu;
    ctx->r[3] = (load_le32(key + 9) >> 6) & 0x3f03fffu;
    ctx->r[4] = (load_le32(key + 12) >> 8) & 0x00fffffu;

    memset(ctx->h, 0, sizeof(ctx->h));

    /* s, the second half, is added once at the end */
    ctx->pad[0] = load_le32(key + 16);
    ctx->pad[1] = load_le32(key + 20);
    ctx->pad[2] = load_le32(key + 24);
    ctx->pad[3] = load_le32(key + 28);

    ctx->used = 0;
}

/* Absorb whole 16-byte blocks.  Each is read as a little-endian number with
 * a 1 bit appended above its top byte (`hibit`, 2^128 in limb terms) - except
 * the padded final partial block, whose appended 1 is a byte of the block
 * itself. */
static void poly1305_blocks(prte_poly1305_ctx_t *ctx, const uint8_t *m, size_t len,
                            uint32_t hibit)
{
    const uint32_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2], r3 = ctx->r[3],
                   r4 = ctx->r[4];
    const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];
    uint64_t d0, d1, d2, d3, d4;
    uint32_t c;

    while (16 <= len) {
        /* h += m */
        h0 += load_le32(m + 0) & LIMB_MASK;
        h1 += (load_le32(m + 3) >> 2) & LIMB_MASK;
        h2 += (load_le32(m + 6) >> 4) & LIMB_MASK;
        h3 += (load_le32(m + 9) >> 6) & LIMB_MASK;
        h4 += (load_le32(m + 12) >> 8) | hibit;

        /* h *= r, folding what lands above 2^130 back in times 5 */
        d0 = (uint64_t) h0 * r0 + (uint64_t) h1 * s4 + (uint64_t) h2 * s3
             + (uint64_t) h3 * s2 + (uint64_t) h4 * s1;
        d1 = (uint64_t) h0 * r1 + (uint64_t) h1 * r0 + (uint64_t) h2 * s4
             + (uint64_t) h3 * s3 + (uint64_t) h4 * s2;
        d2 = (uint64_t) h0 * r2 + (uint64_t) h1 * r1 + (uint64_t) h2 * r0
             + (uint64_t) h3 * s4 + (uint64_t) h4 * s3;
        d3 = (uint64_t) h0 * r3 + (uint64_t) h1 * r2 + (uint64_t) h2 * r1
             + (uint64_t) h3 * r0 + (uint64_t) h4 * s4;
        d4 = (uint64_t) h0 * r4 + (uint64_t) h1 * r3 + (uint64_t) h2 * r2
             + (uint64_t) h3 * r1 + (uint64_t) h4 * r0;

        /* carry back down to 26-bit limbs (partially reduced) */
        c = (uint32_t) (d0 >> 26);
        h0 = (uint32_t) d0 & LIMB_MASK;
        d1 += c;
        c = (uint32_t) (d1 >> 26);
        h1 = (uint32_t) d1 & LIMB_MASK;
        d2 += c;
        c = (uint32_t) (d2 >> 26);
        h2 = (uint32_t) d2 & LIMB_MASK;
        d3 += c;
        c = (uint32_t) (d3 >> 26);
        h3 = (uint32_t) d3 & LIMB_MASK;
        d4 += c;
        c = (uint32_t) (d4 >> 26);
        h4 = (uint32_t) d4 & LIMB_MASK;
        h0 += c * 5;
        c = h0 >> 26;
        h0 &= LIMB_MASK;
        h1 += c;

        m += 16;
        len -= 16;
    }

    ctx->h[0] = h0;
    ctx->h[1] = h1;
    ctx->h[2] = h2;
    ctx->h[3] = h3;
    ctx->h[4] = h4;
}

void prte_poly1305_update(prte_poly1305_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *m = (const uint8_t *) data;
    size_t want;

    /* top up a partial block first */
    if (0 < ctx->used) {
        want = 16 - ctx->used;
        if (want > len) {
            want = len;
        }
        memcpy(ctx->block + ctx->used, m, want);
        ctx->used += want;
        m += want;
        len -= want;
        if (16 > ctx->used) {
            return;
        }
        poly1305_blocks(ctx, ctx->block, 16, 1u << 24);
        ctx->used = 0;
    }
    /* then every whole block straight from the caller's bytes */
    if (16 <= len) {
        want = len & ~(size_t) 15;
        poly1305_blocks(ctx, m, want, 1u << 24);
        m += want;
        len -= want;
    }
    /* and keep what is left */
    if (0 < len) {
        memcpy(ctx->block, m, len);
        ctx->used = len;
    }
}

void prte_poly1305_final(prte_poly1305_ctx_t *ctx, uint8_t tag[PRTE_POLY1305_TAG_LEN])
{
    uint32_t h0, h1, h2, h3, h4, c;
    uint32_t g0, g1, g2, g3, g4, mask;
    uint64_t f;

    /* the last partial block, padded with a 1 byte and then zeros, and with
     * no 2^128 bit of its own */
    if (0 < ctx->used) {
        ctx->block[ctx->used++] = 1;
        memset(ctx->block + ctx->used, 0, 16 - ctx->used);
        poly1305_blocks(ctx, ctx->block, 16, 0);
    }

    /* carry fully */
    h0 = ctx->h[0];
    h1 = ctx->h[1];
    h2 = ctx->h[2];
    h3 = ctx->h[3];
    h4 = ctx->h[4];
    c = h1 >> 26;
    h1 &= LIMB_MASK;
    h2 += c;
    c = h2 >> 26;
    h2 &= LIMB_MASK;
    h3 += c;
    c = h3 >> 26;
    h3 &= LIMB_MASK;
    h4 += c;
    c = h4 >> 26;
    h4 &= LIMB_MASK;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= LIMB_MASK;
    h1 += c;

    /* g = h - p = h + 5 - 2^130; if that did not go negative, h >= p and g
     * is the reduced value.  Chosen by mask rather than by branch. */
    g0 = h0 + 5;
    c = g0 >> 26;
    g0 &= LIMB_MASK;
    g1 = h1 + c;
    c = g1 >> 26;
    g1 &= LIMB_MASK;
    g2 = h2 + c;
    c = g2 >> 26;
    g2 &= LIMB_MASK;
    g3 = h3 + c;
    c = g3 >> 26;
    g3 &= LIMB_MASK;
    g4 = h4 + c - (1u << 26);

    /* all ones when g4 did not borrow (keep g), all zeros when it did */
    mask = (g4 >> 31) - 1;
    g0 &= mask;
    g1 &= mask;
    g2 &= mask;
    g3 &= mask;
    g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    /* h mod 2^128, as four 32-bit words */
    h0 = h0 | (h1 << 26);
    h1 = (h1 >> 6) | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);

    /* tag = (h + s) mod 2^128 */
    f = (uint64_t) h0 + ctx->pad[0];
    h0 = (uint32_t) f;
    f = (uint64_t) h1 + ctx->pad[1] + (f >> 32);
    h1 = (uint32_t) f;
    f = (uint64_t) h2 + ctx->pad[2] + (f >> 32);
    h2 = (uint32_t) f;
    f = (uint64_t) h3 + ctx->pad[3] + (f >> 32);
    h3 = (uint32_t) f;

    store_le32(tag + 0, h0);
    store_le32(tag + 4, h1);
    store_le32(tag + 8, h2);
    store_le32(tag + 12, h3);

    /* the key is one-time: leave nothing of it behind */
    prte_secure_zero(ctx, sizeof(*ctx));
}

void prte_poly1305(const uint8_t key[PRTE_POLY1305_KEY_LEN], const void *msg, size_t len,
                   uint8_t tag[PRTE_POLY1305_TAG_LEN])
{
    prte_poly1305_ctx_t ctx;

    prte_poly1305_init(&ctx, key);
    prte_poly1305_update(&ctx, msg, len);
    prte_poly1305_final(&ctx, tag);
}
