/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * SHA-256 (FIPS 180-4) and HMAC-SHA256 (RFC 2104).
 *
 * These exist for one purpose: daemons of a DVM prove to each other that they
 * hold the DVM's key, without ever sending it (see src/util/prte_dvm_key.h and
 * the connect handshake in src/rml/oob).  PRRTE has no cryptographic library
 * dependency and is built on systems where none is available, so the two
 * primitives that proof needs are carried here rather than taken from one.
 *
 * Nothing here is a general-purpose cryptography API.  There is no streaming
 * HMAC, no other digest, and no encryption: the OOB authenticates its peers,
 * it does not hide or sign the traffic that follows.
 */

#ifndef PRTE_UTIL_HMAC_H
#define PRTE_UTIL_HMAC_H

#include "prte_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

BEGIN_C_DECLS

#define PRTE_SHA256_DIGEST_LEN 32
#define PRTE_SHA256_BLOCK_LEN  64

typedef struct {
    uint32_t h[8];
    uint64_t nbits;
    uint8_t block[PRTE_SHA256_BLOCK_LEN];
    size_t used;
} prte_sha256_ctx_t;

PRTE_EXPORT void prte_sha256_init(prte_sha256_ctx_t *ctx);
PRTE_EXPORT void prte_sha256_update(prte_sha256_ctx_t *ctx, const void *data, size_t len);
PRTE_EXPORT void prte_sha256_final(prte_sha256_ctx_t *ctx, uint8_t out[PRTE_SHA256_DIGEST_LEN]);

/* HMAC-SHA256 of `len` bytes at `msg` under `key`.  A key longer than the
 * block is hashed first, as RFC 2104 requires. */
PRTE_EXPORT void prte_hmac_sha256(const uint8_t *key, size_t keylen, const void *msg, size_t len,
                                  uint8_t out[PRTE_SHA256_DIGEST_LEN]);

/* Compare two buffers in time that does not depend on where they first
 * differ.  Use it - never memcmp - to check a MAC a peer sent: memcmp
 * returns at the first mismatch, which tells a patient guesser how many
 * leading bytes it has right. */
PRTE_EXPORT bool prte_secure_equal(const void *a, const void *b, size_t len);

/* Overwrite `len` bytes in a way the compiler may not drop as a dead store,
 * for scrubbing key material before its memory is released. */
PRTE_EXPORT void prte_secure_zero(void *p, size_t len);

END_C_DECLS

#endif /* PRTE_UTIL_HMAC_H */
