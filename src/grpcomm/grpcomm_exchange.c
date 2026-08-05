/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The allgather exchange schedule.
 *
 * This is pure arithmetic over participant positions - no messages, no state,
 * no PRRTE globals - so it can be reasoned about and tested on its own.  Two
 * different collectives need it: an allgather fence, where every daemon
 * contributes a block and must end holding all of them, and the bulk
 * broadcast, whose second phase is an allgather over the chunks the root
 * scattered.
 *
 * The algorithm is Bruck's.  Recursive doubling is the more obvious choice and
 * is equivalent when the participant count is a power of two, but it *only*
 * works then: for any other count it needs extra rounds that cost up to a
 * second full copy of the data.  Bruck takes ceil(log2(n)) steps for every n,
 * moves each block exactly once, and its per-step partners are as cheap to
 * compute.  The price is that the blocks land rotated - participant `pos` ends
 * up holding its own block first - so the caller must map slots back to owners
 * (prte_grpcomm_bruck_owner) rather than assume natural order.
 *
 * Step k, for a participant at position `pos` among `nprocs`:
 *
 *     send_to    = (pos - 2^k)  mod nprocs
 *     recv_from  = (pos + 2^k)  mod nprocs
 *     nblocks    = min(2^k, nprocs - 2^k)
 *
 * The nblocks clamp is what makes the last step partial when nprocs is not a
 * power of two: without it a participant would send blocks it does not have
 * and the receiver would count blocks twice.
 */

#include "prte_config.h"
#include "constants.h"

#include "grpcomm_internal.h"

size_t prte_grpcomm_bruck_nsteps(size_t nprocs)
{
    size_t steps = 0;
    size_t reach = 1;

    if (2 > nprocs) {
        /* nobody to exchange with - a participant already holds everything
         * there is to hold */
        return 0;
    }
    /* ceil(log2(nprocs)), computed by doubling rather than through a
     * floating-point log: the rounding on log(8)/log(2) is exactly the kind of
     * off-by-one that would drop the last step of an 8-way exchange. */
    while (reach < nprocs) {
        reach *= 2;
        steps++;
    }
    return steps;
}

int prte_grpcomm_bruck_step(size_t pos, size_t nprocs, size_t step,
                            size_t *send_to, size_t *recv_from, size_t *nblocks)
{
    size_t reach;

    if (NULL == send_to || NULL == recv_from || NULL == nblocks) {
        return PRTE_ERR_BAD_PARAM;
    }
    if (2 > nprocs || pos >= nprocs || step >= prte_grpcomm_bruck_nsteps(nprocs)) {
        return PRTE_ERR_BAD_PARAM;
    }

    reach = (size_t) 1 << step;

    /* the modulo is what wraps the exchange into a ring of positions; adding
     * nprocs first keeps the left-hand partner from going negative */
    *send_to = (pos + nprocs - reach) % nprocs;
    *recv_from = (pos + reach) % nprocs;
    *nblocks = (reach < (nprocs - reach)) ? reach : (nprocs - reach);

    return PRTE_SUCCESS;
}

int prte_grpcomm_chunk_bounds(size_t total, size_t nparts, size_t idx,
                              size_t *off, size_t *len)
{
    size_t start, end;

    if (NULL == off || NULL == len) {
        return PRTE_ERR_BAD_PARAM;
    }
    if (0 == nparts || idx >= nparts) {
        return PRTE_ERR_BAD_PARAM;
    }

    /* Bounds are computed from the index rather than accumulated, so chunk i
     * has the same answer on every daemon regardless of which chunks that
     * daemon happens to hold - a scatter hands different pieces to different
     * places, and they have to agree on where the seams are.
     *
     * The multiply is done before the divide so the remainder is spread over
     * the low-numbered chunks instead of being lost: with total=10, nparts=4
     * this gives 2,3,2,3 rather than 2,2,2,2 and four bytes unaccounted for.
     * total is a message size and nparts a daemon count, so the product
     * cannot realistically overflow a size_t; a payload large enough to do
     * that could not have been assembled in the first place. */
    start = (idx * total) / nparts;
    end = ((idx + 1) * total) / nparts;

    *off = start;
    *len = end - start;
    return PRTE_SUCCESS;
}

size_t prte_grpcomm_bruck_owner(size_t pos, size_t nprocs, size_t slot)
{
    if (1 > nprocs || slot >= nprocs) {
        return SIZE_MAX;
    }
    /* Slot 0 is always our own block; each subsequent slot walks forward
     * through the participant positions, wrapping.  This is the rotation the
     * algorithm leaves behind, and undoing it is the caller's job. */
    return (pos + slot) % nprocs;
}
