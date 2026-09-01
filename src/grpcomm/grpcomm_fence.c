/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2007      The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC. All
 *                         rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#include <string.h>

#include "src/class/pmix_list.h"
#include "src/pmix/pmix-internal.h"

#include "src/prted/pmix/pmix_server_internal.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"
#include "src/util/prte_show_help.h"

#include "grpcomm_internal.h"
#include "src/grpcomm/grpcomm.h"

/* internal functions */
static void fence(int sd, short args, void *cbdata);
static prte_grpcomm_fence_t* get_tracker(prte_grpcomm_fence_signature_t *sig,
                                         uint32_t generation, uint32_t step,
                                         bool create);
static int create_dmns(prte_grpcomm_fence_signature_t *sig,
                       pmix_rank_t **dmns, size_t *ndmns);
static int fence_sig_pack(pmix_data_buffer_t *bkt,
                          prte_grpcomm_fence_signature_t *sig);
static int fence_sig_unpack(pmix_data_buffer_t *buffer,
                            prte_grpcomm_fence_signature_t **sig);
static void check_complete(prte_grpcomm_fence_t *coll);
static int fence_op_pack(pmix_data_buffer_t *bkt, prte_grpcomm_fence_op_t op);
static int fence_op_unpack(pmix_data_buffer_t *buffer, prte_grpcomm_fence_op_t *op);
static bool fence_sig_same(prte_grpcomm_fence_signature_t *a,
                           prte_grpcomm_fence_signature_t *b);
static void relcb(void *cbdata);
static void abort_fence_op(prte_grpcomm_fence_t *coll, pmix_status_t st);
static int pack_epoch_frame(pmix_data_buffer_t *framed, pmix_data_buffer_t *body);


/* The rollup: gather to the controller, which broadcasts the answer back. */
static bool tree_gather_converged(prte_grpcomm_fence_t *coll);
static int  tree_gather_contribute(prte_grpcomm_fence_t *coll,
                                   pmix_data_buffer_t *payload);
static void tree_gather_answer(prte_grpcomm_fence_t *coll);

/* PMIX_COLLECT_DATA is the whole of the classification, and its absence is a
 * barrier: a caller that named no directive asked to synchronize and nothing
 * more.  PMIx resolves the flag across a daemon's *local* participants before
 * it ever reaches us - disagreement there becomes PMIX_COLLECT_INVALID and the
 * fence is refused locally - so what arrives here is one answer per daemon,
 * and this is where the DVM-wide agreement gets tested. */
prte_grpcomm_fence_op_t prte_grpcomm_fence_op_from_info(const pmix_info_t info[],
                                                        size_t ninfo)
{
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_COLLECT_DATA)) {
            if (PMIX_INFO_TRUE(&info[n])) {
                return PRTE_GRPCOMM_FENCE_OP_ALLGATHER;
            }
            return PRTE_GRPCOMM_FENCE_OP_BARRIER;
        }
    }
    return PRTE_GRPCOMM_FENCE_OP_BARRIER;
}

bool prte_grpcomm_fence_op_merge(prte_grpcomm_fence_t *coll,
                                 prte_grpcomm_fence_op_t incoming)
{
    /* an arrival that names no operation tells us nothing, so it cannot
     * disagree with anything either */
    if (PRTE_GRPCOMM_FENCE_OP_UNKNOWN == incoming) {
        return true;
    }
    if (PRTE_GRPCOMM_FENCE_OP_UNKNOWN == coll->op) {
        coll->op = incoming;
        return true;
    }
    return (coll->op == incoming);
}

/* Two signatures name the same collective when they name the same procs in
 * the same order. This is the comparison get_tracker() has always made
 * inline; the generation memo needs the same one, so it is named once. */
static bool fence_sig_same(prte_grpcomm_fence_signature_t *a,
                           prte_grpcomm_fence_signature_t *b)
{
    if (a->sz != b->sz) {
        return false;
    }
    if (0 == a->sz) {
        return true;
    }
    if (NULL == a->signature || NULL == b->signature) {
        return (a->signature == b->signature);
    }
    return (0 == memcmp(a->signature, b->signature, a->sz * sizeof(pmix_proc_t)));
}

/* ------------------------------------------------------------------ *
 * Telling one round over a signature from the next.
 *
 * A fence signature is only its participant list, so nothing about a
 * contribution says which round it belongs to.  In the ordinary flow that
 * costs nothing, because a daemon converges only once everything it expects
 * has arrived and so nothing *can* arrive afterwards.  But abort_fence_op()
 * ends a fence early - on a PMIX_TIMEOUT, and on a participant lost with a
 * failed daemon - and a contribution still climbing the tree then reaches a
 * daemon whose tracker the release already retired.  Without a round number
 * fence_recv() builds a fresh tracker for it, and the *next* fence over those
 * same participants finds that tracker, inherits its nreported and its
 * bucket, and can converge early carrying the previous round's data.
 *
 * The group collective solves its version of this with a memo of released
 * operations, and that cannot be copied here: a group is forgotten when a
 * local client starts another of the same name, and a daemon relaying a fence
 * for its subtree has no local client, so the entry would never be forgotten
 * there and the next fence's legitimate contribution would be dropped - a
 * hang in place of a wrong answer.  So this is a counter rather than a memo,
 * and what it counts is releases.
 * ------------------------------------------------------------------ */

static prte_grpcomm_fence_memo_t *fence_gen_find(prte_grpcomm_fence_signature_t *sig)
{
    prte_grpcomm_fence_memo_t *memo;

    PMIX_LIST_FOREACH(memo, &prte_grpcomm_globals.fence_generations,
                      prte_grpcomm_fence_memo_t) {
        if (fence_sig_same(sig, memo->sig)) {
            return memo;
        }
    }
    return NULL;
}

void prte_grpcomm_fence_note_join(bool late)
{
    /* The first wireup settles it.  A later one describes a DVM this daemon
     * is already part of and says nothing about how it got here. */
    if (prte_grpcomm_globals.joined_late_known) {
        return;
    }
    prte_grpcomm_globals.joined_late = late;
    prte_grpcomm_globals.joined_late_known = true;
}

uint32_t prte_grpcomm_fence_gen_baseline(void)
{
    /* A daemon added to a running DVM cannot claim round 0: every daemon that
     * has been present is past it, and would drop a 0 as ancient.  It says it
     * does not know instead, which a receiver takes into whatever round is
     * current - safe precisely because a joiner has no earlier round over
     * this signature to have straggled from, so its first contribution cannot
     * be one.  One contribution per signature, then it has a real number. */
    if (prte_grpcomm_globals.joined_late) {
        return PRTE_GRPCOMM_FENCE_GEN_UNKNOWN;
    }
    /* ...and a daemon that has been here since the start says 0, which is
     * what bootstraps the counter. Without this nothing ever establishes a
     * first round, every contribution is stamped UNKNOWN for ever, and the
     * whole mechanism is inert. */
    return 0;
}

uint32_t prte_grpcomm_fence_gen_next(prte_grpcomm_fence_signature_t *sig)
{
    prte_grpcomm_fence_memo_t *memo = fence_gen_find(sig);

    if (NULL == memo) {
        return prte_grpcomm_fence_gen_baseline();
    }
    return memo->next_generation;
}

bool prte_grpcomm_fence_gen_is_stale(prte_grpcomm_fence_signature_t *sig, uint32_t gen)
{
    uint32_t next;

    /* a contribution that names no round makes no claim to be from an old
     * one either - see the UNKNOWN commentary in fence_recv() */
    if (PRTE_GRPCOMM_FENCE_GEN_UNKNOWN == gen) {
        return false;
    }
    next = prte_grpcomm_fence_gen_next(sig);
    if (PRTE_GRPCOMM_FENCE_GEN_UNKNOWN == next) {
        return false;
    }
    return (gen < next);
}

void prte_grpcomm_fence_gen_record(prte_grpcomm_fence_signature_t *sig, uint32_t gen)
{
    prte_grpcomm_fence_memo_t *memo;

    if (PRTE_GRPCOMM_FENCE_GEN_UNKNOWN == gen) {
        return;
    }

    memo = fence_gen_find(sig);
    if (NULL != memo) {
        /* Adopt rather than increment.  A daemon grown into the DVM after k
         * fences have run has counted none of them, and incrementing would
         * leave it one behind for ever; taking the released generation from
         * the wire puts it in step after its first fence instead.  The test
         * is written so a release that arrives out of order cannot walk the
         * counter backwards. */
        if (memo->next_generation < gen + 1) {
            memo->next_generation = gen + 1;
        }
        return;
    }

    /* Bounded, and evicting the oldest is safe in the way that matters: an
     * entry only has to outlive the messages still in flight for its own
     * fence.  Losing one early does not corrupt anything - it returns that
     * signature to the behaviour this whole mechanism replaced, where a
     * straggler is indistinguishable from the next round. */
    while (PRTE_GRPCOMM_FENCE_MEMO_MAX <=
           pmix_list_get_size(&prte_grpcomm_globals.fence_generations)) {
        memo = (prte_grpcomm_fence_memo_t *)
            pmix_list_remove_first(&prte_grpcomm_globals.fence_generations);
        /* answers NULL for an empty list, and the loop bound is the only
         * thing claiming this one is not - screen it rather than hand a NULL
         * to the reference count */
        if (NULL == memo) {
            break;
        }
        PMIX_RELEASE(memo);
    }

    memo = PMIX_NEW(prte_grpcomm_fence_memo_t);
    if (NULL == memo) {
        return;
    }
    memo->sig = PMIX_NEW(prte_grpcomm_fence_signature_t);
    if (NULL == memo->sig) {
        PMIX_RELEASE(memo);
        return;
    }
    memo->sig->sz = sig->sz;
    if (0 < sig->sz) {
        PMIX_PROC_CREATE(memo->sig->signature, sig->sz);
        if (NULL == memo->sig->signature) {
            PMIX_RELEASE(memo);
            return;
        }
        memcpy(memo->sig->signature, sig->signature, sig->sz * sizeof(pmix_proc_t));
    }
    memo->next_generation = gen + 1;
    pmix_list_append(&prte_grpcomm_globals.fence_generations, &memo->super);
}

/* The generation rides the wire beside the operation, and is checked on
 * arrival rather than only derived locally.  Deriving alone is not enough:
 * every participant does take part in every fence over a set, so counting
 * retirements agrees across daemons that have been present throughout - but a
 * daemon added by an elastic grow has counted none of them, and would
 * originate 0 while everyone else is at k. */
static int fence_gen_pack(pmix_data_buffer_t *bkt, uint32_t gen)
{
    pmix_status_t rc;

    rc = PMIx_Data_pack(NULL, bkt, &gen, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    return PRTE_SUCCESS;
}

static int fence_gen_unpack(pmix_data_buffer_t *buffer, uint32_t *gen)
{
    int32_t cnt = 1;
    pmix_status_t rc;

    rc = PMIx_Data_unpack(NULL, buffer, gen, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    return PRTE_SUCCESS;
}

/* The operation rides every contribution as a byte of its own rather than as
 * one more optional directive, because unlike the timeout and the collective
 * status it is not optional: every contribution has an operation, and a reader
 * that has to cope with its absence cannot tell "barrier" from "nobody said". */
static int fence_op_pack(pmix_data_buffer_t *bkt, prte_grpcomm_fence_op_t op)
{
    uint8_t val = (uint8_t) op;
    pmix_status_t rc;

    rc = PMIx_Data_pack(NULL, bkt, &val, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    return PRTE_SUCCESS;
}

static int fence_op_unpack(pmix_data_buffer_t *buffer, prte_grpcomm_fence_op_t *op)
{
    uint8_t val;
    int32_t cnt = 1;
    pmix_status_t rc;

    rc = PMIx_Data_unpack(NULL, buffer, &val, &cnt, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    /* screen it before it is compared: a value no release ever assigned would
     * otherwise agree with nothing and disagree with nothing, and be taken for
     * a mismatch by every daemon that saw it - which reports a user error for
     * what is a corrupt message */
    if (PRTE_GRPCOMM_FENCE_OP_BARRIER != (prte_grpcomm_fence_op_t) val &&
        PRTE_GRPCOMM_FENCE_OP_ALLGATHER != (prte_grpcomm_fence_op_t) val) {
        return PRTE_ERR_BAD_PARAM;
    }
    *op = (prte_grpcomm_fence_op_t) val;
    return PRTE_SUCCESS;
}

/* Work out how many contributions this daemon has to collect for a fence:
 * one per child subtree holding a participant, plus our own if we are one.
 *
 * create_dmns() has one answer it gives without an array: a signature naming
 * the daemon job itself is "every daemon in the DVM", reported as a count
 * with a NULL array.  There is nothing to walk in that case, but the answer
 * is known exactly - each of our children heads a subtree holding at least
 * one daemon, and we are a participant ourselves.  A NULL array with a zero
 * count is the opposite statement, "no daemons at all", and falls through to
 * the general case, which reads it as zero.
 *
 * Called again on every recovery, because both terms move: a failure can
 * take our children and can take participants. */
static void set_nexpected(prte_grpcomm_fence_t *coll)
{
    size_t n;

    if (NULL == coll->dmns && 0 < coll->ndmns) {
        coll->nexpected = prte_rml_base.n_children + 1;
        return;
    }

    coll->nexpected = prte_rml_get_num_contributors(coll->dmns, coll->ndmns);

    /* see if I am in the array of participants - note that I may
     * be in the rollup tree even though I'm not participating
     * in the collective itself */
    for (n = 0; n < coll->ndmns; n++) {
        if (coll->dmns[n] == PRTE_PROC_MY_NAME->rank) {
            coll->nexpected++;
            break;
        }
    }
}

int prte_grpcomm_fence(const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t info[], size_t ninfo, char *data,
                              size_t ndata, pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_fence_caddy_t *cd;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:fence",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    // bozo check
    if (NULL == procs) {
        return PRTE_ERR_NOT_SUPPORTED;
    }

    cd = PMIX_NEW(prte_pmix_fence_caddy_t);
    cd->procs = (pmix_proc_t*)procs;
    cd->nprocs = nprocs;
    cd->info = (pmix_info_t*)info;
    cd->ninfo = ninfo;
    cd->data = data;
    cd->ndata = ndata;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* must push this into the event library to ensure we can
     * access framework-global data safely */
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, fence, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

/* Frame a contribution with the current recovery epoch. The body is copied,
 * not consumed, so the caller keeps ownership of it. */
static int pack_epoch_frame(pmix_data_buffer_t *framed, pmix_data_buffer_t *body)
{
    pmix_status_t rc;

    rc = PMIx_Data_pack(NULL, framed,
                        &prte_grpcomm_globals.recovery_epoch, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    rc = PMIx_Data_copy_payload(framed, body);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    return PRTE_SUCCESS;
}

/* End an in-flight fence that cannot complete correctly, without taking
 * anything else down with it. It broadcasts a release carrying the signature
 * and a status but no gathered data, which the normal release path hands to
 * each daemon's local participants.
 *
 * The controller calls this - it is the daemon every rollup reaches, so it is
 * the one that can tell a fence that will never converge from one that merely
 * has not yet. */
static void abort_fence_op(prte_grpcomm_fence_t *coll, pmix_status_t st)
{
    pmix_data_buffer_t *reply;
    pmix_status_t prc;
    int rc;

    PMIX_DATA_BUFFER_CREATE(reply);
    rc = fence_sig_pack(reply, coll->sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    prc = PMIx_Data_pack(NULL, reply, &st, 1, PMIX_INT32);
    if (PMIX_SUCCESS != prc) {
        PMIX_ERROR_LOG(prc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    /* An abort is a release like any other and has to say which round it
     * ends - it is in fact the release that makes stragglers possible at
     * all, since it is what ends a fence with contributions still climbing. */
    rc = fence_gen_pack(reply, coll->generation);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    /* xcast copies the payload, so the buffer is still ours to free */
    (void) prte_grpcomm_release_bcast(PRTE_RML_TAG_FENCE_RELEASE, reply);
    PMIX_DATA_BUFFER_RELEASE(reply);
    /* the tracker goes when that release comes back around */
    coll->aborting = true;
}

/* The controller's guard timer for a fence a participant put a deadline on.
 * Firing it completes every participant with PMIX_ERR_TIMEOUT rather than
 * leaving them blocked in PMIx_Fence forever.
 *
 * Nothing else is watching. The PMIx server library arms a timeout of its own
 * while it gathers the local contributions, but deletes it the moment the
 * request is handed to us - deliberately, so that an answer arriving after it
 * fired cannot reach a tracker it has already released. From that point the
 * deadline the caller asked for exists only here. */
static void fence_timeout(int sd, short args, void *cbdata)
{
    prte_grpcomm_fence_t *coll = (prte_grpcomm_fence_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(coll);
    coll->tev_active = false;
    if (coll->converged || coll->aborting) {
        return;
    }
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:fence timeout after %d seconds",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), coll->timeout));
    coll->status = PMIX_ERR_TIMEOUT;
    abort_fence_op(coll, PMIX_ERR_TIMEOUT);
}

void prte_grpcomm_fence_restart(void)
{
    prte_grpcomm_fence_t *coll, *nxt;
    pmix_data_buffer_t *framed;
    int rc;

    PMIX_LIST_FOREACH_SAFE(coll, nxt, &prte_grpcomm_globals.fence_ops,
                           prte_grpcomm_fence_t) {
        if (coll->aborting) {
            continue;
        }
        /* A collective the controller has already answered is finished:
         * its release is on the wire, ordered ahead of anything we could
         * send now, and every daemon will retire its tracker when that
         * release lands. Re-running the rollup here would answer it a
         * second time - a second release broadcast, a second context id
         * consumed, and a second registration of the same group on every
         * daemon. Note this is a test only the controller can apply:
         * "converged" on any other daemon means it rolled its aggregate up
         * to its parent, and re-sending that aggregate is precisely what
         * recovery is for, since the failure may have been what swallowed
         * it. */
        if (PRTE_PROC_IS_MASTER && coll->converged) {
            continue;
        }
        /* A restart: the rollup's shape is the routing tree's, and the tree
         * just changed, so throw away everything gathered under the old one. */
        pmix_bitmap_clear_all_bits(&coll->reported_slots);
        coll->self_reported = false;
        coll->nreported = 0;
        coll->converged = false;
        PMIX_DATA_BUFFER_DESTRUCT(&coll->bucket);
        PMIX_DATA_BUFFER_CONSTRUCT(&coll->bucket);

        /* recompute what the repaired tree owes us - dmns stays the full
         * pre-fault set, and get_num_contributors() already skips the daemons
         * now known to have failed */
        set_nexpected(coll);

        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm:fence restarting at epoch %u, "
                             "nexpected now %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (unsigned) prte_grpcomm_globals.recovery_epoch,
                             (int) coll->nexpected));

        if (NULL == coll->my_contribution) {
            /* nothing of our own to re-offer - we are relaying for our
             * subtree. If that subtree has just gone, nexpected is now zero
             * and this completes on the spot, which is what stops the loss of
             * a pure relay from stalling the fence */
            check_complete(coll);
            continue;
        }

        PMIX_DATA_BUFFER_CREATE(framed);
        rc = pack_epoch_frame(framed, coll->my_contribution);
        if (PRTE_SUCCESS != rc) {
            PMIX_DATA_BUFFER_RELEASE(framed);
            continue;
        }
        PRTE_RML_SEND(rc, PRTE_PROC_MY_NAME->rank, framed, PRTE_RML_TAG_FENCE);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(framed);
        }
    }
}

/* A fence is an allgather with no opt-in to running degraded: its result is
 * every participant's contribution, so losing one means the answer cannot be
 * produced. That is why this differs from the group handler, which can
 * complete on the survivors when asked to.
 *
 * What it must not do is what it used to: kill the job on *any* daemon loss
 * while *any* fence was in flight, without even asking whether the two had
 * anything to do with each other. Fences run constantly, so that made an
 * unrelated daemon failure fatal to bystanders - and because it had no scope
 * guard it also fired twice per failure and once on every revival, taking a
 * job down over a daemon that had just come back. */
void prte_grpcomm_fence_fault_handler(const prte_rml_recovery_status_t* status)
{
    prte_grpcomm_fence_t *coll, *nxt;

    if (0 == pmix_list_get_size(&prte_grpcomm_globals.fence_ops)) {
        return;
    }

    if (PRTE_RML_FAULT_SCOPE_GLOBAL != status->scope) {
        /* A revival arrives here: local scope with no failed ranks, since a
         * death's local pass returns early when it has nothing new. A revival
         * reshapes the tree as a death does, but it rides a forward-first
         * broadcast, so it cannot give the parent-before-child ordering a
         * restart depends on - end the fences instead. */
        if (PRTE_PROC_IS_MASTER && 0 == status->failed_ranks.size) {
            PMIX_LIST_FOREACH_SAFE(coll, nxt, &prte_grpcomm_globals.fence_ops,
                                   prte_grpcomm_fence_t) {
                if (!coll->aborting && !coll->converged) {
                    abort_fence_op(coll, PMIX_ERR_LOST_CONNECTION);
                }
            }
        }
        return;
    }

    if (!PRTE_PROC_IS_MASTER) {
        return;
    }
    PMIX_LIST_FOREACH_SAFE(coll, nxt, &prte_grpcomm_globals.fence_ops,
                           prte_grpcomm_fence_t) {
        /* already answered, or already being torn down */
        if (coll->aborting || coll->converged) {
            continue;
        }
        if (!prte_grpcomm_procs_lost(coll->sig->signature, coll->sig->sz)) {
            /* only the paths between us changed - the restart driven by the
             * component's epoch advance re-converges this one */
            continue;
        }
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm:fence ending a fence that lost a "
                             "participant to a failed daemon",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        abort_fence_op(coll, PMIX_ERR_LOST_CONNECTION);
    }
}

/* Fault injection: carry a held-back contribution across the delay timer.
 * Plain storage rather than a reference-counted object - it lives from the
 * moment the timer is armed until it fires, exactly once, and nothing else
 * ever looks at it. Zeroed on allocation because the embedded event must
 * start clean. */
typedef struct {
    prte_event_t ev;
    pmix_data_buffer_t *framed;
} fence_delay_caddy_t;

static void fence_delay_fire(int sd, short args, void *cbdata)
{
    fence_delay_caddy_t *dc = (fence_delay_caddy_t *) cbdata;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:fence releasing the held-back contribution",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
    PRTE_RML_SEND(rc, PRTE_PROC_MY_NAME->rank, dc->framed, PRTE_RML_TAG_FENCE);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(dc->framed);
    }
    free(dc);
}

/* Should this daemon hold its contribution back?  Only when a delay was asked
 * for and either no vpid was named or this is the one named. */
static bool fence_should_delay(void)
{
    if (0 >= prte_grpcomm_globals.fence_delay_ms) {
        return false;
    }
    if (0 > prte_grpcomm_globals.fence_delay_vpid) {
        return true;
    }
    return ((pmix_rank_t) prte_grpcomm_globals.fence_delay_vpid
            == PRTE_PROC_MY_NAME->rank);
}

static void fence(int sd, short args, void *cbdata)
{
    prte_pmix_fence_caddy_t *cd = (prte_pmix_fence_caddy_t *) cbdata;
    prte_grpcomm_fence_signature_t sig;
    prte_grpcomm_fence_t *coll = NULL;
    int rc;
    /* what our own participants are told if this contribution never makes
     * it into the rollup - PMIX_SUCCESS means it did */
    pmix_status_t st = PMIX_SUCCESS;
    pmix_data_buffer_t *relay, *framed, bkt;
    pmix_byte_object_t bo;
    struct timeval tv;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm: fence",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* compute the signature of this collective */
    PMIX_CONSTRUCT(&sig, prte_grpcomm_fence_signature_t);
    sig.sz = cd->nprocs;
    if (0 < sig.sz) {
        PMIX_PROC_CREATE(sig.signature, sig.sz);
        memcpy(sig.signature, cd->procs, sig.sz * sizeof(pmix_proc_t));
    }

    /* retrieve an existing tracker, create it if not
     * already found. The fence module is responsible
     * for releasing it upon completion of the collective */
    coll = get_tracker(&sig, prte_grpcomm_fence_gen_next(&sig),
                       PRTE_GRPCOMM_FENCE_STEP_ROLLUP, true);
    if (NULL == coll) {
        st = PMIX_ERR_NOT_FOUND;
        goto done;
    }
    coll->cbfunc = cd->cbfunc;
    coll->cbdata = cd->cbdata;

    /* Name the operation from the directives our own participants gave, and
     * fold it in the same way an arriving contribution's would be - this
     * daemon is a participant like any other, and if it is the second one to
     * reach a tracker its answer has to agree with the first. */
    if (!prte_grpcomm_fence_op_merge(coll,
                                     prte_grpcomm_fence_op_from_info(cd->info, cd->ninfo))) {
        prte_show_help("help-prte-grpcomm.txt", "fence-op-mismatch", true,
                       prte_process_info.nodename);
        if (PMIX_SUCCESS == coll->status) {
            coll->status = PMIX_ERR_INVALID_ARG;
        }
    }

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm: fence",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    // execute the fence operation
    PMIX_DATA_BUFFER_CREATE(relay);
    /* pack the signature */
    rc = fence_sig_pack(relay, coll->sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        st = prte_pmix_convert_rc(rc);
        goto done;
    }

    /* say which operation this contribution belongs to */
    rc = fence_op_pack(relay, coll->op);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        st = prte_pmix_convert_rc(rc);
        goto done;
    }

    /* ...and which round of it */
    rc = fence_gen_pack(relay, coll->generation);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        st = prte_pmix_convert_rc(rc);
        goto done;
    }

    // pack the info structs
    rc = PMIx_Data_pack(NULL, relay, &cd->ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        st = rc;
        goto done;
    }
    if (0 < cd->ninfo) {
        rc = PMIx_Data_pack(NULL, relay, cd->info, cd->ninfo, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(relay);
            st = rc;
            goto done;
        }
    }

    /* Pass along the payload - for an allgather, and only for one.
     *
     * A barrier has no data path at all: PMIx still hands us a blob for one
     * (a lone PMIX_COLLECT_NO flag byte, compressed and wrapped), and rolling
     * one of those up from every daemon and broadcasting the concatenation
     * back to all of them is the entire round trip spent on bytes that say
     * only "there is nothing here".  The receiving side wants it no more than
     * we do - PMIx skips its store outright when the host returns no data,
     * where a present-but-empty payload makes it walk the blobs to find that
     * out.
     *
     * An allgather packs its payload unconditionally, including when this
     * daemon has nothing to say.  A participant with an empty contribution is
     * still a participant: it is counted in nexpected, it must report, and
     * with PMIx contributing only what changed an empty block is the ordinary
     * case for any fence after the first rather than a degenerate one. */
    if (PRTE_GRPCOMM_FENCE_OP_ALLGATHER == coll->op) {
        PMIX_DATA_BUFFER_CONSTRUCT(&bkt);
        bo.bytes = cd->data;
        bo.size = cd->ndata;
        PMIx_Data_embed(&bkt, &bo);
        rc = PMIx_Data_copy_payload(relay, &bkt);
        PMIX_DATA_BUFFER_DESTRUCT(&bkt);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(relay);
            st = rc;
            goto done;
        }
    }

    /* Keep our own contribution so a fault can replay it: recovery resets
     * every tracker and has each daemon re-offer what it originally gave.
     * PRTE_RML_SEND takes ownership of what we hand it, so this is a copy. */
    if (NULL != coll->my_contribution) {
        PMIX_DATA_BUFFER_RELEASE(coll->my_contribution);
    }
    PMIX_DATA_BUFFER_CREATE(coll->my_contribution);
    rc = PMIx_Data_copy_payload(coll->my_contribution, relay);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(coll->my_contribution);
        coll->my_contribution = NULL;
        PMIX_DATA_BUFFER_RELEASE(relay);
        st = rc;
        goto done;
    }

    /* stamp it with the current epoch and send that */
    PMIX_DATA_BUFFER_CREATE(framed);
    rc = pack_epoch_frame(framed, relay);
    PMIX_DATA_BUFFER_RELEASE(relay);
    if (PRTE_SUCCESS != rc) {
        PMIX_DATA_BUFFER_RELEASE(framed);
        st = prte_pmix_convert_rc(rc);
        goto done;
    }

    /* send this to ourselves for processing */
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:fence sending to ourself",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    if (fence_should_delay()) {
        /* Hold it back, so that a fence ended without it - by a PMIX_TIMEOUT,
         * or by losing a participant - has this contribution still on the
         * wire when its release lands. That is the straggler the generation
         * has to recognize, and it is otherwise unreachable from a test.
         *
         * The entry point already answered PRTE_SUCCESS, so nothing upstream
         * is waiting on this send; if the fence is aborted meanwhile our own
         * participants are completed by the release, exactly as they would be
         * for a contribution genuinely lost in the network. */
        fence_delay_caddy_t *dc = (fence_delay_caddy_t *) calloc(1, sizeof(*dc));
        if (NULL == dc) {
            PMIX_DATA_BUFFER_RELEASE(framed);
            st = PMIX_ERR_NOMEM;
            goto done;
        }
        dc->framed = framed;
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm:fence holding its contribution back %d ms",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             prte_grpcomm_globals.fence_delay_ms));
        tv.tv_sec = prte_grpcomm_globals.fence_delay_ms / 1000;
        tv.tv_usec = (prte_grpcomm_globals.fence_delay_ms % 1000) * 1000;
        prte_event_evtimer_set(prte_event_base, &dc->ev, fence_delay_fire, dc);
        PMIX_POST_OBJECT(dc);
        prte_event_evtimer_add(&dc->ev, &tv);
    } else {
        PRTE_RML_SEND(rc, PRTE_PROC_MY_NAME->rank, framed,
                      PRTE_RML_TAG_FENCE);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(framed);
            st = prte_pmix_convert_rc(rc);
        }
    }

done:
    /* the signature we computed is ours - the tracker keeps a copy of its
     * own - so it must go back on every path out of here */
    PMIX_DESTRUCT(&sig);

    if (PMIX_SUCCESS != st) {
        /* Our contribution never entered the rollup, so no release is coming
         * to complete the clients waiting in PMIx_Fence - and this daemon
         * returned PRTE_SUCCESS from the entry point, so nothing upstream
         * knows to fail them either. Complete them here with the reason, and
         * take them off the tracker first so that a release arriving later
         * (the controller can still abort the fence) cannot complete them a
         * second time. The tracker itself is left in place: it is what any
         * such release still has to find. */
        if (NULL != coll) {
            coll->cbfunc = NULL;
            coll->cbdata = NULL;
        }
        if (NULL != cd->cbfunc) {
            cd->cbfunc(st, NULL, 0, cd->cbdata, NULL, NULL);
        }
    }
    PMIX_RELEASE(cd);
}

void prte_grpcomm_fence_recv(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tag, void *cbdata)
{
    int32_t cnt;
    int rc, timeout, slot;
    uint32_t stamp;
    struct timeval tv;
    size_t n, ninfo;
    pmix_status_t st;
    pmix_info_t *info = NULL;
    prte_grpcomm_fence_signature_t *sig = NULL;
    prte_grpcomm_fence_t *coll;
    prte_grpcomm_fence_op_t op;
    uint32_t gen;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm fence recvd from %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(sender)));

    /* every message on this tag opens with the sender's recovery epoch */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &stamp, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (stamp < prte_grpcomm_globals.recovery_epoch) {
        /* sent before a failure this daemon has already recovered from, so it
         * belongs to a round that no longer exists */
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm fence stale epoch %u (at %u) - dropping",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned) stamp,
                             (unsigned) prte_grpcomm_globals.recovery_epoch));
        return;
    }
    if (stamp > prte_grpcomm_globals.recovery_epoch) {
        /* should be unreachable - see the recovery_epoch commentary in
         * grpcomm_internal.h - but adopting it is what our own fault notice
         * would do a moment later, so do that rather than lose the message */
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_ORDER_MSG);
        prte_grpcomm_advance_epoch(stamp);
    }

    /* unpack the signature */
    rc = fence_sig_unpack(buffer, &sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        /* sig is NULL on failure - bail rather than deref it in get_tracker */
        return;
    }

    /* Which operation and which round this contribution belongs to.  Both are
     * read before the tracker is looked up, because whether we want a tracker
     * at all depends on the round. */
    rc = fence_op_unpack(buffer, &op);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_RELEASE(sig);
        return;
    }
    rc = fence_gen_unpack(buffer, &gen);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_RELEASE(sig);
        return;
    }

    /* A contribution from a round we have already released.  This is the
     * straggler abort_fence_op() makes possible: its fence was ended early,
     * the release retired our tracker, and this was still climbing the tree.
     *
     * Dropping it here, before get_tracker(), is the whole point.  Creating a
     * tracker for it and then discarding the message would leave exactly the
     * wreck this mechanism exists to prevent - a tracker carrying a stale
     * contribution that the *next* fence over these participants would find,
     * inherit, and converge early on. */
    if (prte_grpcomm_fence_gen_is_stale(sig, gen)) {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm fence dropping a contribution from "
                             "generation %u (now at %u)",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned) gen,
                             (unsigned) prte_grpcomm_fence_gen_next(sig)));
        PMIX_RELEASE(sig);
        return;
    }

    /* Which round this contribution belongs to, and so which tracker it joins.
     *
     * A sender that named a round gets that round - including one AHEAD of
     * where we are.  That happens routinely rather than exceptionally: xcast
     * hands a release to our children before we process it ourselves, so a
     * child can be released, open the next round, and reach us while we are
     * still finishing the previous one.  Its contribution gets a tracker of
     * its own and accumulates there until our own release arrives and we
     * catch up.  Folding it into the round we are still in - which is what
     * adopting the higher number onto the live tracker would do - would put
     * the next round's data in a bucket holding this round's.
     *
     * A sender that named no round is a daemon a grow added, which has not
     * learned this signature's numbering yet.  It joins whatever round we
     * think is current, which is the right answer for a joiner and cannot be
     * a straggler: it has no earlier round here to have straggled from. */
    if (PRTE_GRPCOMM_FENCE_GEN_UNKNOWN == gen) {
        gen = prte_grpcomm_fence_gen_next(sig);
    }
    if (NULL == (coll = get_tracker(sig, gen, PRTE_GRPCOMM_FENCE_STEP_ROLLUP, true))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PMIX_RELEASE(sig);
        return;
    }
    PMIX_RELEASE(sig);

    if (!prte_grpcomm_fence_op_merge(coll, op)) {
        /* The participants asked for different collectives.  Report it and
         * carry on: the contribution still has to be counted or the fence
         * never converges, and it is convergence that carries the failure
         * back out to every participant through the release.  This is the
         * same treatment a non-success PMIX_LOCAL_COLLECTIVE_STATUS gets, and
         * the status is deliberately the one PMIx answers for the local form
         * of the same disagreement. */
        prte_show_help("help-prte-grpcomm.txt", "fence-op-mismatch", true,
                       prte_process_info.nodename);
        if (PMIX_SUCCESS == coll->status) {
            coll->status = PMIX_ERR_INVALID_ARG;
        }
    }

    /* Identify which child subtree this came from, and drop it whole if that
     * subtree has already been heard from - see the matching note in the group
     * path. Only the *test* happens here: the accounting is committed below,
     * once the message has parsed, so a truncated one cannot leave a subtree
     * counted with none of its data merged. */
    slot = -1;
    if (sender->rank == PRTE_PROC_MY_NAME->rank) {
        if (coll->self_reported) {
            return;
        }
    } else {
        slot = prte_rml_get_subtree_index(sender->rank);
        if (0 > slot) {
            /* not in any of our subtrees - we are not this daemon's parent,
             * so this contribution is not ours to aggregate */
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            return;
        }
        if (pmix_bitmap_is_set_bit(&coll->reported_slots, slot)) {
            return;
        }
    }

    // unpack the info structs
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        cnt = ninfo;
        rc = PMIx_Data_unpack(NULL, buffer, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(info, ninfo);
            return;
        }
    }

    /* Merge the directives this contribution carried into the tracker. The
     * array itself is freed unread below - what gets forwarded upward is
     * rebuilt from the tracker in tree_gather_answer(), so writing the merged
     * values back into these entries would say nothing to anybody. */
    for (n=0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
            rc = PMIx_Value_get_number(&info[n].value, &timeout, PMIX_INT);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_FREE(info, ninfo);
                return;
            }
            if (coll->timeout < timeout) {
                coll->timeout = timeout;
            }

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_LOCAL_COLLECTIVE_STATUS)) {
            rc = PMIx_Value_get_number(&info[n].value, &st, PMIX_STATUS);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_FREE(info, ninfo);
                return;
            }
            if (PMIX_SUCCESS != st &&
                PMIX_SUCCESS == coll->status) {
                coll->status = st;
            }
        }
    }

    /* Arm the deadline, if a participant asked for one.  Only the controller
     * does: it is the one daemon every contribution reaches, so it is the only
     * one that can tell a fence that will never converge from one that simply
     * has not yet. */
    if (!coll->tev_active && 0 < coll->timeout && PRTE_PROC_IS_MASTER) {
        prte_event_evtimer_set(prte_event_base, &coll->tev, fence_timeout, coll);
        tv.tv_sec = coll->timeout;
        tv.tv_usec = 0;
        coll->tev_active = true;
        PMIX_POST_OBJECT(coll);
        prte_event_evtimer_add(&coll->tev, &tv);
    }

    /* Absorb the payload - the remainder of the message after the info
     * structs - into the bucket.  A barrier put none there, and asking for it
     * anyway would be asking a fully-consumed buffer for its remainder.
     *
     * Note what is *not* conditional: the accounting below.  Participation is
     * counted, never weighed, so a contribution of zero bytes advances the
     * rollup exactly as far as a large one does.  That is what lets an
     * allgather stay an allgather when a participant has nothing to add. */
    if (PRTE_GRPCOMM_FENCE_OP_ALLGATHER == coll->op) {
        rc = tree_gather_contribute(coll, buffer);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_INFO_FREE(info, ninfo);
            return;
        }
    }
    PMIX_INFO_FREE(info, ninfo);

    /* the contribution is in - now commit the accounting for it */
    if (0 > slot) {
        coll->self_reported = true;
    } else {
        pmix_bitmap_set_bit(&coll->reported_slots, slot);
    }
    coll->nreported++;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm fence recv nexpected %d nrep %d",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) coll->nexpected,
                         (int) coll->nreported));

    check_complete(coll);
}

/* Test whether this fence's rollup is complete and, if so, answer it: the
 * controller broadcasts the release, everyone else rolls their bucket up to
 * their parent. Factored out of fence_recv() because a contribution arriving
 * is no longer the only thing that can complete a fence - a fault can lower
 * what we are waiting for, and the tracker has to be re-tested with no
 * message in hand.
 *
 * The info array forwarded upward is rebuilt from the tracker's own merged
 * state rather than echoed from whichever contribution happened to arrive
 * last. Only the timeout and the local collective status are ever read out of
 * it, and both are accumulated on the tracker, so this says the same thing
 * without depending on a message being present. */
static void check_complete(prte_grpcomm_fence_t *coll)
{

    if (coll->converged || coll->aborting) {
        return;
    }

    if (!tree_gather_converged(coll)) {
        return;
    }

    coll->converged = true;
    /* the fence resolved, so the guard timer has done its job */
    if (coll->tev_active) {
        prte_event_del(&coll->tev);
        coll->tev_active = false;
    }
    tree_gather_answer(coll);
}

static bool tree_gather_converged(prte_grpcomm_fence_t *coll)
{
    return coll->nreported >= coll->nexpected;
}

/* The rollup's bucket is an append: order is whatever the merges happened to
 * reach us in, which is why the result is not reproducible across daemons. */
static int tree_gather_contribute(prte_grpcomm_fence_t *coll,
                                  pmix_data_buffer_t *payload)
{
    int rc;

    if (NULL == payload) {
        return PRTE_SUCCESS;
    }
    rc = PMIx_Data_copy_payload(&coll->bucket, payload);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    return PRTE_SUCCESS;
}

/* MOVEMENT: roll the contributions up the routing tree to the controller,
 * which broadcasts the gathered result back down.
 *
 * Right for a barrier, where there is nothing to gather and the cost is the
 * depth of the tree in each direction. Its weakness is the release: the
 * controller alone holds the answer, so it must fan the whole payload back out
 * over the tree, and for a full modex that fanout is essentially the entire
 * cost of the collective. */
static void tree_gather_answer(prte_grpcomm_fence_t *coll)
{
    pmix_data_buffer_t *reply, *framed;
    pmix_info_t *info = NULL;
    size_t ninfo = 0;
    int rc;

    if (PRTE_PROC_IS_MASTER) {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm fence HNP reports complete",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        PMIX_DATA_BUFFER_CREATE(reply);
        rc = fence_sig_pack(reply, coll->sig);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
        rc = PMIx_Data_pack(NULL, reply, &coll->status, 1, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
        /* Which round is being ended.  Carrying it down as well as up is what
         * lets a daemon that has not been present for every fence over these
         * participants - one grown into the DVM - adopt the true number
         * instead of counting from its own arrival. */
        rc = fence_gen_pack(reply, coll->generation);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
        /* A barrier's release is the signature and the status: there is
         * nothing gathered to hand back, and saying so by sending nothing is
         * what lets every daemon's release path skip the unload and PMIx skip
         * its store.  The operation is not on this message because it does
         * not need to be - a daemon only reaches its release path by holding
         * a tracker, and a tracker knows which collective it is. */
        if (PRTE_GRPCOMM_FENCE_OP_BARRIER != coll->op) {
            rc = PMIx_Data_copy_payload(reply, &coll->bucket);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                return;
            }
        }
        /* xcast copies the payload, so the buffer is still ours to free */
        (void) prte_grpcomm_release_bcast(PRTE_RML_TAG_FENCE_RELEASE, reply);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm fence rollup complete - sending to %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(PRTE_PROC_MY_PARENT)));

    PMIX_DATA_BUFFER_CREATE(reply);
    rc = fence_sig_pack(reply, coll->sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }

    /* the aggregate travels as a contribution like any other, so it names its
     * operation like any other - this is the merged answer for our whole
     * subtree, which is what our parent has to agree with */
    rc = fence_op_pack(reply, coll->op);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    rc = fence_gen_pack(reply, coll->generation);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }

    /* rebuild the directives we accumulated */
    if (0 < coll->timeout) {
        ++ninfo;
    }
    if (PMIX_SUCCESS != coll->status) {
        ++ninfo;
    }
    if (0 < ninfo) {
        size_t idx = 0;
        PMIX_INFO_CREATE(info, ninfo);
        if (0 < coll->timeout) {
            PMIX_INFO_LOAD(&info[idx++], PMIX_TIMEOUT, &coll->timeout, PMIX_INT);
        }
        if (PMIX_SUCCESS != coll->status) {
            PMIX_INFO_LOAD(&info[idx++], PMIX_LOCAL_COLLECTIVE_STATUS,
                           &coll->status, PMIX_STATUS);
        }
    }
    rc = PMIx_Data_pack(NULL, reply, &ninfo, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_INFO_FREE(info, ninfo);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    if (0 < ninfo) {
        rc = PMIx_Data_pack(NULL, reply, info, ninfo, PMIX_INFO);
        PMIX_INFO_FREE(info, ninfo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
    }

    if (PRTE_GRPCOMM_FENCE_OP_BARRIER != coll->op) {
        rc = PMIx_Data_copy_payload(reply, &coll->bucket);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
    }

    /* stamp it with the epoch it belongs to, so a parent that has already
     * recovered past it can tell it is stale */
    PMIX_DATA_BUFFER_CREATE(framed);
    rc = pack_epoch_frame(framed, reply);
    PMIX_DATA_BUFFER_RELEASE(reply);
    if (PRTE_SUCCESS != rc) {
        PMIX_DATA_BUFFER_RELEASE(framed);
        return;
    }
    PRTE_RML_SEND(rc, PRTE_PROC_MY_PARENT->rank, framed, PRTE_RML_TAG_FENCE);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(framed);
    }
}

/* ---------------------------------------------------------------------- */
static void relcb(void *cbdata)
{
    uint8_t *data = (uint8_t *) cbdata;

    if (NULL != data) {
        free(data);
    }
}

void prte_grpcomm_fence_release(int status, pmix_proc_t *sender,
                                       pmix_data_buffer_t *buffer,
                                       prte_rml_tag_t tag, void *cbdata)
{
    int32_t cnt;
    int rc, ret;
    prte_grpcomm_fence_signature_t *sig = NULL;
    prte_grpcomm_fence_t *coll;
    pmix_byte_object_t bo;
    uint32_t gen;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_globals.output,
                         "%s grpcomm: fence release called with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) buffer->bytes_used));

    /* unpack the signature */
    rc = fence_sig_unpack(buffer, &sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the return status */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ret, &cnt, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(sig);
        return;
    }

    /* Which round this release ends, and remember it.
     *
     * This happens whether or not we hold a tracker.  A daemon with no
     * participants of its own still needs the count, because it may relay for
     * a subtree in a later round and has to be able to recognize a straggler
     * from this one.  It is also what a daemon grown into the DVM adopts
     * instead of counting rounds from its own arrival. */
    rc = fence_gen_unpack(buffer, &gen);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_RELEASE(sig);
        return;
    }
    prte_grpcomm_fence_gen_record(sig, gen);

    /* Find the round this release ends.  Not an error if there is none - that
     * just means we had no participants in it.
     *
     * The fallback is for a daemon a grow added.  It did not know this
     * signature's numbering when it opened its tracker, so that tracker is
     * filed under UNKNOWN rather than under the round the rest of the DVM
     * calls this one; the release is the first thing that tells it, so adopt
     * the number onto it here.  There can only be the one such tracker per
     * signature: the next round opens with a real number, because recording
     * this release above gave us one. */
    coll = get_tracker(sig, gen, PRTE_GRPCOMM_FENCE_STEP_ROLLUP, false);
    if (NULL == coll) {
        coll = get_tracker(sig, PRTE_GRPCOMM_FENCE_GEN_UNKNOWN,
                           PRTE_GRPCOMM_FENCE_STEP_ROLLUP, false);
        if (NULL != coll) {
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                                 "%s grpcomm:fence learning that its round is %u",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned) gen));
            coll->generation = gen;
        }
    }
    if (NULL == coll) {
        PMIX_RELEASE(sig);
        return;
    }

    /* Unload the buffer. An aborted fence carries no gathered data, so an
     * empty or unreadable payload is expected there - do not let that
     * overwrite the status the controller sent, which is the whole message.
     *
     * A barrier is not asked at all.  Its release legitimately carries nothing
     * on the success path too, so an unload that objected to an empty buffer
     * would turn every barrier into a failure; and there is nothing to gain by
     * asking, because handing PMIx a NULL payload is what tells it to skip the
     * store rather than walk a bucket to discover it is empty.  The test is
     * written so that only a positively-known barrier is skipped: a tracker
     * still at UNKNOWN never heard a contribution, which is a released fence
     * this daemon merely relayed for, and taking the old path there costs
     * nothing. */
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
    if (PRTE_GRPCOMM_FENCE_OP_BARRIER != coll->op) {
        rc = PMIx_Data_unload(buffer, &bo);
        if (PMIX_SUCCESS != rc && PMIX_SUCCESS == ret) {
            ret = rc;
        }
    }

    /* Retire the tracker BEFORE delivering, not after. This fence is over the
     * moment we have its result; everything below is delivery, and delivery is
     * exactly when the next fence can start. The callback completes the local
     * clients' PMIx_Fence, and a client is free to fence again over the same
     * participants as soon as it returns - a signature is only its participant
     * list, so that next fence looks up the very tracker we are about to free
     * and would join a collective that has already been answered.
     *
     * Taking it off the list first costs nothing: we still hold the reference
     * that keeps `coll` alive for the callback below, and nothing between here
     * and the release needs to find it by lookup. The abort path above does
     * the same thing for the same reason, by clearing cbfunc rather than by
     * unlinking. */
    pmix_list_remove_item(&prte_grpcomm_globals.fence_ops, &coll->super);

    /* execute the callback */
    if (NULL != coll->cbfunc) {
        coll->cbfunc(ret, bo.bytes, bo.size, coll->cbdata, relcb, bo.bytes);
    } else {
        /* Nobody here was waiting on this fence: we hold a tracker because we
         * relayed for our subtree, not because we had a participant of our
         * own. The gathered data is still ours to free - relcb is the only
         * other thing that frees it, and it only ever runs because the
         * callback above was handed it. */
        PMIX_BYTE_OBJECT_DESTRUCT(&bo);
    }
    PMIX_RELEASE(coll);
    PMIX_RELEASE(sig);
}

/* Does this tracker have the identity we are looking for?
 *
 * All three parts, and the reason each is here is in the tracker's own
 * definition. Note that the generation is compared exactly, including when
 * it is UNKNOWN: a daemon that has not learned a signature's numbering keeps
 * its work on an UNKNOWN tracker and adopts a real number when a release
 * tells it one - see the note in fence_release(). */
static bool tracker_matches(prte_grpcomm_fence_t *coll,
                            prte_grpcomm_fence_signature_t *sig,
                            uint32_t generation, uint32_t step)
{
    if (coll->generation != generation || coll->step != step) {
        return false;
    }
    return fence_sig_same(sig, coll->sig);
}

static prte_grpcomm_fence_t* get_tracker(prte_grpcomm_fence_signature_t *sig,
                                         uint32_t generation, uint32_t step,
                                         bool create)
{
    prte_grpcomm_fence_t *coll;
    int rc;

    /* search the existing tracker list to see if this already exists */
    PMIX_LIST_FOREACH(coll, &prte_grpcomm_globals.fence_ops, prte_grpcomm_fence_t) {
        if (tracker_matches(coll, sig, generation, step)) {
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                                 "%s grpcomm:base:returning existing collective",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
            return coll;
        }
    }
    /* if we get here, then this is a new collective - so create
     * the tracker for it */
    if (!create) {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                             "%s grpcomm:base: not creating new coll",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

        return NULL;
    }
    coll = PMIX_NEW(prte_grpcomm_fence_t);
    // we have to know the participating procs
    coll->sig = PMIX_NEW(prte_grpcomm_fence_signature_t);
    coll->sig->sz = sig->sz;
    if (0 < coll->sig->sz) {
        PMIX_PROC_CREATE(coll->sig->signature, coll->sig->sz);
        memcpy(coll->sig->signature, sig->signature, coll->sig->sz * sizeof(pmix_proc_t));
    }
    /* the identity we were asked for - the caller resolved it, because only
     * the caller knows whether it is opening a round of its own or joining
     * one somebody else named */
    coll->generation = generation;
    coll->step = step;
    pmix_list_append(&prte_grpcomm_globals.fence_ops, &coll->super);

    /* now get the daemons involved */
    if (PRTE_SUCCESS != (rc = create_dmns(sig, &coll->dmns, &coll->ndmns))) {
        PRTE_ERROR_LOG(rc);
        /* a tracker with no daemon set can never be completed, and leaving it
         * on the list is worse than losing it: the next fence of the same
         * signature would find this one, see a rollup that expects nothing,
         * and answer with data it never gathered */
        pmix_list_remove_item(&prte_grpcomm_globals.fence_ops, &coll->super);
        PMIX_RELEASE(coll);
        return NULL;
    }

    /* count the number of contributions we should get */
    set_nexpected(coll);

    return coll;
}

/* Same, for a caller outside this file. Exported only so the unit test can
 * build a tracker and inspect what the rollup was sized to expect, which is
 * where a fence goes wrong long before any message moves. */
prte_grpcomm_fence_t *prte_grpcomm_fence_get_tracker(prte_grpcomm_fence_signature_t *sig,
                                                            uint32_t generation,
                                                            uint32_t step,
                                                            bool create)
{
    return get_tracker(sig, generation, step, create);
}

static int create_dmns(prte_grpcomm_fence_signature_t *sig,
                       pmix_rank_t **dmns, size_t *ndmns)
{
    size_t n;
    prte_job_t *jdata;
    prte_proc_t *proc;
    prte_node_t *node;
    prte_job_map_t *map;
    int i;
    pmix_list_t ds;
    prte_namelist_t *nm;
    pmix_rank_t vpid;
    bool found;
    size_t nds = 0;
    pmix_rank_t *dns = NULL;
    int rc = PRTE_SUCCESS;
    /* Once the DVM has known failures, a participant we cannot resolve is
     * most likely one whose node is being torn down. Refusing to resolve the
     * whole set would strand the fence, because the caller drops the
     * contribution with no completion path - so skip what we cannot place and
     * carry on. With nothing failed, an unresolvable participant is still a
     * genuine error and still fatal. */
    bool tolerate = !pmix_bitmap_is_clear(&prte_rml_base.failed_dmns);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:fence:create_dmns called with %s signature size %" PRIsize_t "",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == sig->signature) ? "NULL" : "NON-NULL", sig->sz));

    /* A signature has to name somebody. This is not reachable from a local
     * client - the PMIx server hands us the participants it aggregated - but
     * the signature also arrives off the wire, where a truncated message
     * unpacks to an empty one. The test below would then read signature[0]
     * of an array that is NULL; and an entry with no nspace is worse than
     * that, because PMIX_CHECK_NSPACE answers "yes" for an empty nspace
     * against anything, so it would be taken for a fence over the daemon job
     * and sized to expect the whole DVM. Refuse it: the caller drops the
     * message, which is the right answer for one we cannot read. */
    if (0 == sig->sz || NULL == sig->signature ||
        PMIX_NSPACE_INVALID(sig->signature[0].nspace)) {
        *dmns = NULL;
        *ndmns = 0;
        return PRTE_ERR_BAD_PARAM;
    }

    /* if the target jobid is our own,
     * then all daemons are participating */
    if (PMIX_CHECK_NSPACE(PRTE_PROC_MY_NAME->nspace, sig->signature[0].nspace)) {
        *ndmns = prte_process_info.num_daemons;
        *dmns = NULL;
        return PRTE_SUCCESS;
    }

    PMIX_CONSTRUCT(&ds, pmix_list_t);
    for (n = 0; n < sig->sz; n++) {
        if (NULL == (jdata = prte_get_job_data_object(sig->signature[n].nspace))) {
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            rc = PRTE_ERR_NOT_FOUND;
            break;
        }
        map = (prte_job_map_t*)jdata->map;
        if (NULL == map || 0 == map->num_nodes) {
            /* we haven't generated a job map yet - if we are the HNP,
             * then we should only involve ourselves. Otherwise, we have
             * no choice but to abort to avoid hangs */
            if (PRTE_PROC_IS_MASTER) {
                rc = PRTE_SUCCESS;
                break;
            }
            PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
            rc = PRTE_ERR_NOT_FOUND;
            break;
        }
        if (PMIX_RANK_WILDCARD == sig->signature[n].rank) {
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                                 "%s grpcomm:fence::create_dmns called for all procs in job %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(sig->signature[0].nspace)));
            /* all daemons hosting this jobid are participating */
            for (i = 0; i < map->nodes->size; i++) {
                if (NULL == (node = pmix_pointer_array_get_item(map->nodes, i))) {
                    continue;
                }
                if (NULL == node->daemon) {
                    if (tolerate) {
                        continue;
                    }
                    PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                    rc = PRTE_ERR_NOT_FOUND;
                    goto done;
                }
                found = false;
                PMIX_LIST_FOREACH(nm, &ds, prte_namelist_t)
                {
                    if (nm->name.rank == node->daemon->name.rank) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_globals.output,
                                         "%s grpcomm:fence::create_dmns adding daemon %s to list",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                         PRTE_NAME_PRINT(&node->daemon->name)));
                    nm = PMIX_NEW(prte_namelist_t);
                    PMIX_LOAD_PROCID(&nm->name, PRTE_PROC_MY_NAME->nspace, node->daemon->name.rank);
                    pmix_list_append(&ds, &nm->super);
                }
            }
        } else {
            /* lookup the daemon for this proc and add it to the list */
            PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_globals.output,
                                 "%s sign: GETTING PROC OBJECT FOR %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&sig->signature[n])));
            proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs,
                                                               sig->signature[n].rank);
            if (NULL == proc) {
                if (tolerate) {
                    continue;
                }
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                rc = PRTE_ERR_NOT_FOUND;
                goto done;
            }
            if (NULL == proc->node || NULL == proc->node->daemon) {
                if (tolerate) {
                    continue;
                }
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                rc = PRTE_ERR_NOT_FOUND;
                goto done;
            }
            vpid = proc->node->daemon->name.rank;
            found = false;
            PMIX_LIST_FOREACH(nm, &ds, prte_namelist_t)
            {
                if (nm->name.rank == vpid) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                nm = PMIX_NEW(prte_namelist_t);
                PMIX_LOAD_PROCID(&nm->name, PRTE_PROC_MY_NAME->nspace, vpid);
                pmix_list_append(&ds, &nm->super);
            }
        }
    }

done:
    if (0 < pmix_list_get_size(&ds)) {
        dns = (pmix_rank_t *) malloc(pmix_list_get_size(&ds) * sizeof(pmix_rank_t));
        nds = 0;
        while (NULL != (nm = (prte_namelist_t *) pmix_list_remove_first(&ds))) {
            PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_globals.output,
                                 "%s grpcomm:fence::create_dmns adding daemon %s to array",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&nm->name)));
            dns[nds++] = nm->name.rank;
            PMIX_RELEASE(nm);
        }
    }
    PMIX_LIST_DESTRUCT(&ds);
    *dmns = dns;
    *ndmns = nds;
    return rc;
}

static int fence_sig_pack(pmix_data_buffer_t *bkt,
                          prte_grpcomm_fence_signature_t *sig)
{
    pmix_status_t rc;

    // always send the participating procs
    rc = PMIx_Data_pack(NULL, bkt, &sig->sz, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 < sig->sz) {
        rc = PMIx_Data_pack(NULL, bkt, sig->signature, sig->sz, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    return PRTE_SUCCESS;
}

static int fence_sig_unpack(pmix_data_buffer_t *buffer,
                            prte_grpcomm_fence_signature_t **sig)
{
    pmix_status_t rc;
    int32_t cnt;
    prte_grpcomm_fence_signature_t *s;

    s = PMIX_NEW(prte_grpcomm_fence_signature_t);

    // unpack the participating procs
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->sz, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }
    if (0 < s->sz) {
        PMIX_PROC_CREATE(s->signature, s->sz);
        cnt = s->sz;
        rc = PMIx_Data_unpack(NULL, buffer, s->signature, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(s);
            return prte_pmix_convert_status(rc);
        }
    }

    *sig = s;
    return PRTE_SUCCESS;
}
