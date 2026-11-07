/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Poly1305 (RFC 8439, section 2.5): a one-time authenticator.
 *
 * It is here for the OOB's optional per-message tags
 * (prte_oob_authenticate_messages, src/rml/oob): each message a daemon sends
 * on a connection carries a Poly1305 tag under a key used for that message
 * alone.  Poly1305 is what makes tagging every message affordable - it runs
 * at several times the rate of the HMAC-SHA256 in prte_hmac.[ch], which is
 * kept to deriving the keys.
 *
 * A Poly1305 key must never authenticate two different messages: once a key
 * has tagged two, its tags no longer show that a message is unaltered.  The
 * caller derives a fresh key per message; nothing here can check that it
 * did.
 */

#ifndef PRTE_UTIL_POLY1305_H
#define PRTE_UTIL_POLY1305_H

#include "prte_config.h"

#include <stddef.h>
#include <stdint.h>

BEGIN_C_DECLS

#define PRTE_POLY1305_KEY_LEN 32
#define PRTE_POLY1305_TAG_LEN 16

typedef struct {
    uint32_t r[5];   /* the clamped multiplier, in 26-bit limbs */
    uint32_t h[5];   /* the accumulator, in 26-bit limbs */
    uint32_t pad[4]; /* the second half of the key, added at the end */
    uint8_t block[16];
    size_t used;     /* bytes held in block */
} prte_poly1305_ctx_t;

PRTE_EXPORT void prte_poly1305_init(prte_poly1305_ctx_t *ctx,
                                    const uint8_t key[PRTE_POLY1305_KEY_LEN]);
PRTE_EXPORT void prte_poly1305_update(prte_poly1305_ctx_t *ctx, const void *data, size_t len);
/* write the tag, and wipe the context */
PRTE_EXPORT void prte_poly1305_final(prte_poly1305_ctx_t *ctx,
                                     uint8_t tag[PRTE_POLY1305_TAG_LEN]);

/* the tag of `len` bytes at `msg` under `key`, in one call */
PRTE_EXPORT void prte_poly1305(const uint8_t key[PRTE_POLY1305_KEY_LEN], const void *msg,
                               size_t len, uint8_t tag[PRTE_POLY1305_TAG_LEN]);

END_C_DECLS

#endif /* PRTE_UTIL_POLY1305_H */
