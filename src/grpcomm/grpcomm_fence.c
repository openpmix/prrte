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
static prte_grpcomm_fence_t* get_tracker(prte_grpcomm_fence_signature_t *sig, bool create);
static int create_dmns(prte_grpcomm_fence_signature_t *sig,
                       pmix_rank_t **dmns, size_t *ndmns);
static int fence_sig_pack(pmix_data_buffer_t *bkt,
                          prte_grpcomm_fence_signature_t *sig);
static int fence_sig_unpack(pmix_data_buffer_t *buffer,
                            prte_grpcomm_fence_signature_t **sig);
static void check_complete(prte_grpcomm_fence_t *coll);
static void relcb(void *cbdata);
static void abort_fence_op(prte_grpcomm_fence_t *coll, pmix_status_t st);
static int pack_epoch_frame(pmix_data_buffer_t *framed, pmix_data_buffer_t *body);


/* How a converged fence is answered.
 *
 * The framing around this - the signature, the tracker and its lifetime,
 * create_dmns(), my_contribution and the replay it feeds, the recovery epoch,
 * abort_fence_op(), the timeout accumulation, and the completion callback into
 * the PMIx server - is the same whichever way the contributions travelled, and
 * there is one implementation of it. A movement supplies data transport only.
 *
 * The seam is narrower than the broadcast's. A fence is a rollup followed by a
 * release, and it is the *release* that a different movement changes: gathering
 * to a controller obliges it to broadcast the answer back, while a true
 * allgather leaves every daemon already holding the result with nothing to
 * release at all. So a movement is, at bottom, "what to do once this tracker
 * has everything it is waiting for". */
typedef struct {
    uint32_t id;                                /* stamped on the wire */
    const char *name;
    /* has this tracker got everything it is waiting for? the rollup counts
     * child subtrees, the exchange counts blocks - they are not the same
     * question, so it is asked through here */
    bool (*converged)(prte_grpcomm_fence_t *coll);
    /* absorb this daemon's own contribution, and set whatever the movement
     * needs in motion */
    int (*contribute)(prte_grpcomm_fence_t *coll, pmix_data_buffer_t *payload);
    void (*answer)(prte_grpcomm_fence_t *coll); /* the tracker has converged */
} fence_movement_t;

/* rd_allgather's per-tracker state.
 *
 * Participants are `coll->dmns` in ascending rank order, which every daemon
 * derives from the same signature through the same create_dmns() - so unlike
 * the broadcast there is no list to carry, and unlike the broadcast there is
 * also nobody whose answer is authoritative. That is why the movement id is
 * on the wire: it is the only way a disagreement becomes visible. */
typedef struct fence_exchange_t {
    pmix_rank_t *parts;          /* participant ranks, ascending */
    size_t nparts;
    size_t mypos;                /* our position, SIZE_MAX if not a participant */
    pmix_byte_object_t *blocks;  /* [nparts], indexed by POSITION */
    bool *held;                  /* [nparts]; an empty block is still a block */
    size_t nheld;
    size_t step;                 /* next Bruck step to run */
    bool started;                /* our own block is in and the exchange is live */
} fence_exchange_t;

static bool tree_gather_converged(prte_grpcomm_fence_t *coll);
static int  tree_gather_contribute(prte_grpcomm_fence_t *coll,
                                   pmix_data_buffer_t *payload);
static void tree_gather_answer(prte_grpcomm_fence_t *coll);
static bool rd_allgather_converged(prte_grpcomm_fence_t *coll);
static int  rd_allgather_contribute(prte_grpcomm_fence_t *coll,
                                    pmix_data_buffer_t *payload);
static void rd_allgather_answer(prte_grpcomm_fence_t *coll);

static const fence_movement_t fence_movements[] = {
    { .id = PRTE_GRPCOMM_FENCE_TREE_GATHER,
      .name = "tree_gather_release",
      .converged = tree_gather_converged,
      .contribute = tree_gather_contribute,
      .answer = tree_gather_answer },
    { .id = PRTE_GRPCOMM_FENCE_RD_ALLGATHER,
      .name = "rd_allgather",
      .converged = rd_allgather_converged,
      .contribute = rd_allgather_contribute,
      .answer = rd_allgather_answer },
};

static const fence_movement_t* fence_movement_by_id(uint32_t id)
{
    for (size_t i = 0; i < sizeof(fence_movements)/sizeof(fence_movements[0]); i++) {
        if (fence_movements[i].id == id) {
            return &fence_movements[i];
        }
    }
    return NULL;
}

/* Which movement should carry this fence?
 *
 * Every participant answers this independently and they must agree, so it may
 * only be computed from things every participant sees identically: the
 * configured selection, and the directives the caller passed - which PMIx
 * requires to be uniform across a fence's participants, and enforces within a
 * node. Anything derived from purely local state (how many bytes *our* clients
 * contributed, say) would be a way to hang the collective.
 *
 * "auto" reads PMIX_COLLECT_DATA, which is the only thing that distinguishes a
 * modex from a barrier. A barrier keeps the rollup: with nothing to gather,
 * the tree costs two traversals of a message with no payload, and there is
 * nothing an allgather could improve. A modex is the case the allgather exists
 * for, because it is the release fanout - not the gather - that dominates. */
static uint32_t select_fence_movement(prte_grpcomm_fence_t *coll,
                                      const pmix_info_t *info, size_t ninfo)
{
    bool collect = false;

    if (PRTE_GRPCOMM_FENCE_SELECT_AUTO != prte_grpcomm_globals.fence_select) {
        return prte_grpcomm_globals.fence_select;
    }
    /* an exchange needs at least two participants to be an exchange */
    if (2 > coll->ndmns) {
        return PRTE_GRPCOMM_FENCE_TREE_GATHER;
    }
    for (size_t n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_COLLECT_DATA)) {
            collect = PMIX_INFO_TRUE(&info[n]);
            break;
        }
    }
    return collect ? PRTE_GRPCOMM_FENCE_RD_ALLGATHER
                   : PRTE_GRPCOMM_FENCE_TREE_GATHER;
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
 * Mostly the controller calls this - it is the daemon every rollup reaches, so
 * it is the one that can tell a fence that will never converge from one that
 * merely has not yet. But a movement disagreement is visible to whichever
 * daemon spots it and is fatal to the whole collective, so that path calls it
 * from wherever it is noticed; the broadcast reaches the controller by the
 * ordinary relay either way. */
static void abort_fence_op(prte_grpcomm_fence_t *coll, pmix_status_t st)
{
    pmix_data_buffer_t *reply;
    pmix_status_t rc;

    PMIX_DATA_BUFFER_CREATE(reply);
    rc = fence_sig_pack(reply, coll->sig);
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    rc = PMIx_Data_pack(NULL, reply, &st, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
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
        /* An exchange has nothing to restart. The restart exists because a
         * rollup's shape is the routing tree's, and the tree just changed; an
         * exchange's shape is the participant list, which a repaired tree does
         * not touch. Blocks already collected are still exactly the blocks
         * their owners contributed, and re-offering our own would only
         * duplicate what our partners already hold. If the failure took a
         * participant, the fault handler has already ended this fence - and if
         * it did not, the exchange is still running correctly. */
        if (PRTE_GRPCOMM_FENCE_RD_ALLGATHER == coll->movement) {
            continue;
        }

        /* throw away everything gathered under the old tree */
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

    /* An exchange is judged locally, on every participant, because there is
     * no daemon that all of it reaches - and for a fence over a subset the
     * controller may not even be in it. Each participant asks whether the
     * exchange it is running has lost anybody; if it has, the exchange can
     * never close, because the block that participant owed exists nowhere
     * else. There is no "lost only a relay" case to consider here at all:
     * relay-only daemons are not in an exchange. */
    PMIX_LIST_FOREACH_SAFE(coll, nxt, &prte_grpcomm_globals.fence_ops,
                           prte_grpcomm_fence_t) {
        if (coll->aborting || coll->converged ||
            PRTE_GRPCOMM_FENCE_RD_ALLGATHER != coll->movement ||
            NULL == coll->xch) {
            continue;
        }
        for (size_t i = 0; i < coll->xch->nparts; i++) {
            if (!pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns,
                                        coll->xch->parts[i])) {
                continue;
            }
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                                 "%s grpcomm:fence:allgather lost participant "
                                 "%s - ending the fence",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_VPID_PRINT(coll->xch->parts[i])));
            /* Complete our own participants and drop the tracker. No
             * broadcast: every other participant is reaching the same
             * conclusion from the same failed-daemon set at the same time. */
            if (NULL != coll->cbfunc) {
                coll->cbfunc(PMIX_ERR_LOST_CONNECTION, NULL, 0, coll->cbdata,
                             NULL, NULL);
            }
            pmix_list_remove_item(&prte_grpcomm_globals.fence_ops, &coll->super);
            PMIX_RELEASE(coll);
            break;
        }
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
        if (PRTE_GRPCOMM_FENCE_RD_ALLGATHER == coll->movement) {
            /* judged above, locally, and either ended there or unaffected */
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
    coll = get_tracker(&sig, true);
    if (NULL == coll) {
        st = PMIX_ERR_NOT_FOUND;
        goto done;
    }
    coll->cbfunc = cd->cbfunc;
    coll->cbdata = cd->cbdata;

    /* Choose how this fence will travel, from the directive the caller
     * actually gave. PMIX_COLLECT_DATA is what separates a barrier from a
     * modex, and contrary to long-standing belief it *does* reach us: the
     * client packs the caller's info array verbatim and the PMIx server hands
     * that array straight to this upcall. (The obvious substitute - "is
     * ndata zero" - would have been wrong anyway. A pure barrier arrives here
     * with eight bytes of payload, not none.)
     *
     * Only a tracker we are creating for our own client may be steered this
     * way. One that already has contributions in it has adopted a movement
     * from whoever spoke first, and changing it underneath them is exactly
     * the disagreement the interlock exists to catch. */
    if (0 == coll->nreported && !coll->self_reported) {
        coll->movement = select_fence_movement(coll, cd->info, cd->ninfo);
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

    /* Say how we intend to move this fence's contributions. Nobody downstream
     * obeys it - every participant decides for itself, because there is no
     * originator to decide for them - so this is not an instruction but an
     * assertion, and the receiver's job is to notice if it disagrees. Packed
     * unguarded: every daemon in a DVM runs the same build, so the bytes are
     * never conditional even when the behaviour is. */
    rc = PMIx_Data_pack(NULL, relay, &coll->movement, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        st = rc;
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

    /* pass along the payload */
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

    PRTE_RML_SEND(rc, PRTE_PROC_MY_NAME->rank, framed,
                  PRTE_RML_TAG_FENCE);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(framed);
        st = prte_pmix_convert_rc(rc);
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
    uint32_t stamp, movement;
    struct timeval tv;
    size_t n, ninfo;
    pmix_status_t st;
    pmix_info_t *info = NULL;
    prte_grpcomm_fence_signature_t *sig = NULL;
    prte_grpcomm_fence_t *coll;
    const fence_movement_t *mv;
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

    /* The movement this contribution was gathered under. Read before the
     * tracker is consulted so that the buffer stays in step whatever happens
     * next - the fields after it are laid out the same way regardless. */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &movement, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(sig);
        return;
    }

    /* check for the tracker and create it if not found */
    if (NULL == (coll = get_tracker(sig, true))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PMIX_RELEASE(sig);
        return;
    }
    PMIX_RELEASE(sig);

    /* A fence has no originator to decide how it travels, so every
     * participant works it out independently and they must all reach the same
     * answer. If they do not, the collective cannot converge: half the
     * daemons wait for a release that the other half will never send. That is
     * the one catastrophic failure mode of this design, and the whole reason
     * the movement is on the wire - to turn it into a report rather than a
     * DVM-wide hang with nothing to look at.
     *
     * A brand-new tracker adopts what it is told: the tracker this daemon
     * builds on the first arriving contribution has no opinion of its own
     * yet, and the sender's assertion is the only information available. Our
     * own local contribution, when it comes, is checked against it like any
     * other. */
    if (NULL == fence_movement_by_id(movement)) {
        prte_show_help("help-prte-grpcomm.txt", "fence-movement-unknown", true,
                       prte_process_info.nodename, (unsigned long) movement);
        abort_fence_op(coll, PMIX_ERR_NOT_SUPPORTED);
        return;
    }
    if (0 == coll->nreported && !coll->self_reported) {
        coll->movement = movement;
    } else if (coll->movement != movement) {
        const fence_movement_t *ours = fence_movement_by_id(coll->movement);
        const fence_movement_t *theirs = fence_movement_by_id(movement);
        prte_show_help("help-prte-grpcomm.txt", "fence-movement-mismatch", true,
                       prte_process_info.nodename,
                       (NULL == ours) ? "unknown" : ours->name,
                       PRTE_NAME_PRINT(sender),
                       (NULL == theirs) ? "unknown" : theirs->name);
        abort_fence_op(coll, PMIX_ERR_NOT_SUPPORTED);
        return;
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

    /* cycle thru the info to look for keys we support */
    for (n=0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, timeout, int);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_FREE(info, ninfo);
                return;
            }
            if (coll->timeout < timeout) {
                coll->timeout = timeout;
            }
            /* update the info with the collected value */
            info[n].value.type = PMIX_INT;
            info[n].value.data.integer = coll->timeout;

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_LOCAL_COLLECTIVE_STATUS)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, st, pmix_status_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_FREE(info, ninfo);
                return;
            }
            if (PMIX_SUCCESS != st &&
                PMIX_SUCCESS == coll->status) {
                coll->status = st;
            }
            /* update the info with the collected value */
            info[n].value.type = PMIX_STATUS;
            info[n].value.data.status = coll->status;
        }
    }

    /* Arm the deadline, if a participant asked for one.
     *
     * Under the rollup only the controller does this: it is the one daemon
     * every contribution reaches, so it is the only one that can tell a fence
     * that will never converge from one that simply has not yet. An exchange
     * has no such daemon - and for a fence over a subset the controller may
     * not even be a participant - so there the deadline is each participant's
     * own to keep, and whoever fires first aborts for everybody. */
    if (!coll->tev_active && 0 < coll->timeout &&
        (PRTE_PROC_IS_MASTER ||
         PRTE_GRPCOMM_FENCE_RD_ALLGATHER == coll->movement)) {
        prte_event_evtimer_set(prte_event_base, &coll->tev, fence_timeout, coll);
        tv.tv_sec = coll->timeout;
        tv.tv_usec = 0;
        coll->tev_active = true;
        PMIX_POST_OBJECT(coll);
        prte_event_evtimer_add(&coll->tev, &tv);
    }

    /* Hand the payload to the movement. Under the rollup this appends to the
     * bucket; under an exchange it is our own block and starts our half of
     * the schedule. Either way what arrives here is the same bytes - the
     * remainder of the message after the info structs. */
    mv = fence_movement_by_id(coll->movement);
    if (NULL == mv) {
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        PMIX_INFO_FREE(info, ninfo);
        return;
    }
    rc = mv->contribute(coll, buffer);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_INFO_FREE(info, ninfo);
        return;
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
    const fence_movement_t *mv;

    if (coll->converged || coll->aborting) {
        return;
    }

    mv = fence_movement_by_id(coll->movement);
    if (NULL == mv) {
        /* Unreachable from the wire - fence_recv refuses an id it does not
         * implement before it ever reaches a tracker - so this is a local
         * bug rather than a bad message, and the fence cannot be answered. */
        PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
        abort_fence_op(coll, PMIX_ERR_NOT_SUPPORTED);
        return;
    }
    /* "Has it got everything" is itself movement-dependent: the rollup is
     * counting child subtrees, the exchange is counting blocks. */
    if (!mv->converged(coll)) {
        return;
    }

    coll->converged = true;
    /* the fence resolved, so the guard timer has done its job */
    if (coll->tev_active) {
        prte_event_del(&coll->tev);
        coll->tev_active = false;
    }
    mv->answer(coll);
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
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
        rc = PMIx_Data_pack(NULL, reply, &coll->status, 1, PMIX_INT32);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }
        rc = PMIx_Data_copy_payload(reply, &coll->bucket);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
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
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    /* the movement rides with every contribution on this tag, including the
     * aggregate we are rolling up, so our parent can check it against its own */
    rc = PMIx_Data_pack(NULL, reply, &coll->movement, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
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

    rc = PMIx_Data_copy_payload(reply, &coll->bucket);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
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
/* MOVEMENT: a lateral allgather, with no release at all.
 *
 * The rollup's weakness is not the gather, it is the release: only the
 * controller ends up holding the answer, so it has to fan the whole payload
 * back down the tree, and for a full modex that fanout is essentially the
 * entire cost of the collective.  An allgather leaves every participant
 * already holding the result, so there is nothing to release - the two halves
 * collapse into one primitive.
 *
 * Participants are coll->dmns in ascending rank order.  Every daemon derives
 * that from the same signature through the same create_dmns(), so unlike the
 * broadcast there is no list to carry.  What there also is not, unlike the
 * broadcast, is anybody whose choice is authoritative - which is why the
 * movement id travels on the wire and a disagreement is a reported error.
 *
 * Blocks land rotated (Bruck), so reassembly maps slots back through
 * prte_grpcomm_bruck_owner() and concatenates in ascending POSITION order.
 * That is a real improvement over the rollup, whose bucket ends up in
 * whatever order the merges happened to reach the root: every daemon here
 * produces byte-identical output. */

void prte_grpcomm_fence_xch_free(prte_grpcomm_fence_t *coll)
{
    fence_exchange_t *x = coll->xch;

    if (NULL == x) {
        return;
    }
    if (NULL != x->blocks) {
        for (size_t i = 0; i < x->nparts; i++) {
            PMIX_BYTE_OBJECT_DESTRUCT(&x->blocks[i]);
        }
        free(x->blocks);
    }
    if (NULL != x->held) {
        free(x->held);
    }
    if (NULL != x->parts) {
        free(x->parts);
    }
    free(x);
    coll->xch = NULL;
}

/* Materialize the participant list.  create_dmns() has one answer it gives
 * without an array - a signature naming the daemon job is "every daemon",
 * reported as a count with a NULL array - and an exchange needs the ranks
 * themselves, so that case is expanded here. */
static int xch_attach(prte_grpcomm_fence_t *coll)
{
    fence_exchange_t *x;
    size_t n = 0;

    prte_grpcomm_fence_xch_free(coll);
    x = (fence_exchange_t *) calloc(1, sizeof(fence_exchange_t));
    if (NULL == x) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    x->mypos = SIZE_MAX;

    if (0 == coll->ndmns) {
        free(x);
        return PRTE_ERR_BAD_PARAM;
    }
    x->parts = (pmix_rank_t *) malloc(coll->ndmns * sizeof(pmix_rank_t));
    if (NULL == x->parts) {
        free(x);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    /* A daemon-job fence is "every daemon in the DVM", which create_dmns()
     * reports as a count with no array - so the ranks are the count, expanded
     * here because an exchange needs the identities.
     *
     * Either way the list is filtered through prte_rml_is_node_up(). A DVM
     * that has shrunk or lost a daemon keeps the hole forever (vpids are
     * never reused), and num_daemons still counts it - so without this an
     * exchange would sit waiting on a block from a rank nobody can reach.
     * The rollup does not need the filter because its shape comes from the
     * repaired routing tree, which already routes around the hole; the
     * exchange's shape is this list, so the filter has to be here. It reads
     * the same failed-daemon set on every daemon, which is what keeps the
     * participants agreeing. */
    if (NULL == coll->dmns) {
        for (pmix_rank_t r = 0; r < (pmix_rank_t) coll->ndmns; r++) {
            if (prte_rml_is_node_up(r)) {
                x->parts[n++] = r;
            }
        }
    } else {
        for (size_t i = 0; i < coll->ndmns; i++) {
            if (prte_rml_is_node_up(coll->dmns[i])) {
                x->parts[n++] = coll->dmns[i];
            }
        }
    }
    if (0 == n) {
        free(x->parts);
        free(x);
        return PRTE_ERR_BAD_PARAM;
    }
    x->nparts = n;

    x->blocks = (pmix_byte_object_t *) calloc(n ? n : 1, sizeof(pmix_byte_object_t));
    x->held = (bool *) calloc(n ? n : 1, sizeof(bool));
    if (NULL == x->blocks || NULL == x->held) {
        if (NULL != x->blocks) {
            free(x->blocks);
        }
        if (NULL != x->held) {
            free(x->held);
        }
        free(x->parts);
        free(x);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    for (size_t i = 0; i < n; i++) {
        if (x->parts[i] == PRTE_PROC_MY_NAME->rank) {
            x->mypos = i;
            break;
        }
    }
    coll->xch = x;
    return PRTE_SUCCESS;
}

/* Store one block at its position.  Idempotent, because a replayed step can
 * re-deliver a block we already hold and nheld is what decides completion. */
static int xch_store(fence_exchange_t *x, size_t pos, const pmix_byte_object_t *src)
{
    if (pos >= x->nparts || x->held[pos]) {
        return PRTE_SUCCESS;
    }
    if (0 == src->size) {
        /* a real but empty block - a barrier's contribution is exactly this,
         * so it must be distinguishable from "absent" by the slot map rather
         * than by the pointer */
        x->blocks[pos].bytes = NULL;
        x->blocks[pos].size = 0;
    } else {
        x->blocks[pos].bytes = (char *) malloc(src->size);
        if (NULL == x->blocks[pos].bytes) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        memcpy(x->blocks[pos].bytes, src->bytes, src->size);
        x->blocks[pos].size = src->size;
    }
    x->held[pos] = true;
    x->nheld++;
    return PRTE_SUCCESS;
}

static bool xch_have_blocks(const fence_exchange_t *x, size_t nblocks)
{
    for (size_t j = 0; j < nblocks; j++) {
        size_t owner = prte_grpcomm_bruck_owner(x->mypos, x->nparts, j);
        if (SIZE_MAX == owner || !x->held[owner]) {
            return false;
        }
    }
    return true;
}

/* Send the blocks one exchange step owes its partner. */
static int xch_send_step(prte_grpcomm_fence_t *coll, size_t send_to, size_t nblocks)
{
    fence_exchange_t *x = coll->xch;
    pmix_rank_t dest = x->parts[send_to];
    pmix_data_buffer_t *buf, *framed;
    int rc;

    PMIX_DATA_BUFFER_CREATE(buf);
    rc = fence_sig_pack(buf, coll->sig);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, buf, &coll->movement, 1, PMIX_UINT32);
    }
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Data_pack(NULL, buf, &nblocks, 1, PMIX_SIZE);
    }
    for (size_t j = 0; PMIX_SUCCESS == rc && j < nblocks; j++) {
        size_t owner = prte_grpcomm_bruck_owner(x->mypos, x->nparts, j);
        rc = PMIx_Data_pack(NULL, buf, &owner, 1, PMIX_SIZE);
        if (PMIX_SUCCESS == rc) {
            rc = PMIx_Data_pack(NULL, buf, &x->blocks[owner], 1, PMIX_BYTE_OBJECT);
        }
    }
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(buf);
        return rc;
    }

    /* stamped with the epoch like every other collective message, so a
     * partner that has already recovered past it can tell it is stale */
    PMIX_DATA_BUFFER_CREATE(framed);
    rc = pack_epoch_frame(framed, buf);
    PMIX_DATA_BUFFER_RELEASE(buf);
    if (PRTE_SUCCESS != rc) {
        PMIX_DATA_BUFFER_RELEASE(framed);
        return rc;
    }

    /* the partner is chosen by its position in the exchange, not by where it
     * sits in the routing tree, so tell the RML this link is not a lifeline
     * before the send opens it */
    prte_rml_lateral_register(dest);

    PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_globals.output,
                         "%s grpcomm:fence:allgather sending %d blocks to %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) nblocks,
                         PRTE_VPID_PRINT(dest)));

    PRTE_RML_SEND_DIRECT(rc, dest, framed, PRTE_RML_TAG_FENCE_EXCHANGE);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(framed);
        return rc;
    }
    return PRTE_SUCCESS;
}

/* Run every step we now hold the blocks for.  A loop rather than one step per
 * arriving message, because steps can land out of order. */
static void xch_progress(prte_grpcomm_fence_t *coll)
{
    fence_exchange_t *x = coll->xch;
    size_t nsteps;

    if (NULL == x || !x->started || SIZE_MAX == x->mypos) {
        return;
    }
    nsteps = prte_grpcomm_bruck_nsteps(x->nparts);
    while (x->step < nsteps) {
        size_t send_to, recv_from, nblocks;

        if (PRTE_SUCCESS != prte_grpcomm_bruck_step(x->mypos, x->nparts, x->step,
                                                    &send_to, &recv_from, &nblocks)) {
            break;
        }
        if (!xch_have_blocks(x, nblocks)) {
            break;
        }
        if (PRTE_SUCCESS != xch_send_step(coll, send_to, nblocks)) {
            /* the exchange cannot close without this step, and there is no
             * controller holding a spare copy to replay it */
            abort_fence_op(coll, PMIX_ERR_LOST_CONNECTION);
            return;
        }
        x->step++;
    }
}

static bool rd_allgather_converged(prte_grpcomm_fence_t *coll)
{
    return NULL != coll->xch && coll->xch->started &&
           coll->xch->nheld >= coll->xch->nparts;
}

/* Our own contribution: stand the exchange up, keep our block, and start. */
static int rd_allgather_contribute(prte_grpcomm_fence_t *coll,
                                   pmix_data_buffer_t *payload)
{
    pmix_byte_object_t bo = PMIX_BYTE_OBJECT_STATIC_INIT;
    pmix_data_buffer_t rest;
    int rc;

    if (NULL == coll->xch) {
        rc = xch_attach(coll);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
    }
    if (SIZE_MAX == coll->xch->mypos) {
        /* We are relaying for a fence we are not in. Under the rollup that is
         * an ordinary role; under an exchange there is nothing to relay, so
         * there is simply nothing for us to do. */
        return PRTE_SUCCESS;
    }
    if (coll->xch->started) {
        return PRTE_SUCCESS;
    }

    /* Our block is whatever is left after the info structs - the same bytes
     * the rollup would have appended to its bucket.
     *
     * It has to be taken with PMIx_Data_copy_payload, NOT read off base_ptr /
     * bytes_used: those describe the WHOLE buffer, and this one has already
     * had its epoch, signature, movement and info unpacked out of it. Copying
     * from the base would put that framing inside every daemon's block and
     * hand the assembled result to PMIx as garbage. copy_payload copies from
     * the unpack cursor, which is exactly the remainder. */
    PMIX_DATA_BUFFER_CONSTRUCT(&rest);
    if (NULL != payload) {
        rc = PMIx_Data_copy_payload(&rest, payload);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&rest);
            return prte_pmix_convert_status(rc);
        }
    }
    if (0 < rest.bytes_used) {
        bo.bytes = rest.base_ptr;
        bo.size = rest.bytes_used;
    }
    rc = xch_store(coll->xch, coll->xch->mypos, &bo);  /* takes a copy */
    PMIX_DATA_BUFFER_DESTRUCT(&rest);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    coll->xch->started = true;
    xch_progress(coll);
    return PRTE_SUCCESS;
}

/* Everyone already holds everything, so there is no release: assemble in
 * ascending participant order and hand the result to our own clients. */
static void rd_allgather_answer(prte_grpcomm_fence_t *coll)
{
    fence_exchange_t *x = coll->xch;
    pmix_byte_object_t out = PMIX_BYTE_OBJECT_STATIC_INIT;
    size_t total = 0, at = 0;

    for (size_t i = 0; i < x->nparts; i++) {
        total += x->blocks[i].size;
    }
    if (0 < total) {
        out.bytes = (char *) malloc(total);
        if (NULL == out.bytes) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            abort_fence_op(coll, PMIX_ERR_NOMEM);
            return;
        }
        /* ascending POSITION order, which is ascending rank order - the same
         * on every daemon, which is the property the rollup never had */
        for (size_t i = 0; i < x->nparts; i++) {
            if (0 < x->blocks[i].size) {
                memcpy(out.bytes + at, x->blocks[i].bytes, x->blocks[i].size);
                at += x->blocks[i].size;
            }
        }
        out.size = total;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                         "%s grpcomm:fence:allgather complete with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) total));

    if (NULL != coll->cbfunc) {
        coll->cbfunc(coll->status, out.bytes, out.size, coll->cbdata,
                     relcb, out.bytes);
    } else {
        /* nobody local was waiting - we only hold a tracker because we are a
         * participant with no clients of our own */
        PMIX_BYTE_OBJECT_DESTRUCT(&out);
    }
    pmix_list_remove_item(&prte_grpcomm_globals.fence_ops, &coll->super);
    PMIX_RELEASE(coll);
}

/* Blocks arriving from an exchange partner. */
void prte_grpcomm_fence_exchange_recv(int status, pmix_proc_t *sender,
                                      pmix_data_buffer_t *buffer,
                                      prte_rml_tag_t tag, void *cbdata)
{
    int32_t cnt;
    int rc;
    uint32_t stamp, movement;
    size_t nblocks;
    prte_grpcomm_fence_signature_t *sig = NULL;
    prte_grpcomm_fence_t *coll;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &stamp, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (stamp < prte_grpcomm_globals.recovery_epoch) {
        return;
    }
    if (stamp > prte_grpcomm_globals.recovery_epoch) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_ORDER_MSG);
        prte_grpcomm_advance_epoch(stamp);
    }

    rc = fence_sig_unpack(buffer, &sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return;
    }
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &movement, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(sig);
        return;
    }

    /* Create the tracker if it is not there yet: a partner nearer the start of
     * the schedule can reach us before our own client has called PMIx_Fence,
     * and its blocks are exactly what we would otherwise have to ask for
     * again. The tracker is what holds them until our own contribution
     * arrives and starts our half of the exchange. */
    coll = get_tracker(sig, true);
    PMIX_RELEASE(sig);
    if (NULL == coll) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return;
    }
    if (coll->converged || coll->aborting) {
        return;
    }
    if (PRTE_GRPCOMM_FENCE_RD_ALLGATHER != movement) {
        prte_show_help("help-prte-grpcomm.txt", "fence-movement-mismatch", true,
                       prte_process_info.nodename, "rd_allgather",
                       PRTE_NAME_PRINT(sender), "tree_gather_release");
        abort_fence_op(coll, PMIX_ERR_NOT_SUPPORTED);
        return;
    }
    coll->movement = movement;
    if (NULL == coll->xch) {
        rc = xch_attach(coll);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            abort_fence_op(coll, prte_pmix_convert_rc(rc));
            return;
        }
    }

    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &nblocks, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    for (size_t i = 0; i < nblocks; i++) {
        pmix_byte_object_t blk = PMIX_BYTE_OBJECT_STATIC_INIT;
        size_t pos = 0;

        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &pos, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS == rc) {
            cnt = 1;
            rc = PMIx_Data_unpack(NULL, buffer, &blk, &cnt, PMIX_BYTE_OBJECT);
        }
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return;
        }
        rc = xch_store(coll->xch, pos, &blk);
        PMIX_BYTE_OBJECT_DESTRUCT(&blk);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            abort_fence_op(coll, prte_pmix_convert_rc(rc));
            return;
        }
    }

    xch_progress(coll);
    check_complete(coll);
}

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
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_globals.output,
                         "%s grpcomm: fence release called with %d bytes",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) buffer->bytes_used));

    /* unpack the signature */
    rc = fence_sig_unpack(buffer, &sig);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
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

    /* check for the tracker - it is not an error if not
     * found as that just means we are not involved
     * in the collective */
    if (NULL == (coll = get_tracker(sig, false))) {
        PMIX_RELEASE(sig);
        return;
    }

    /* unload the buffer. An aborted fence carries no gathered data, so an
     * empty or unreadable payload is expected there - do not let that
     * overwrite the status the controller sent, which is the whole message. */
    PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
    rc = PMIx_Data_unload(buffer, &bo);
    if (PMIX_SUCCESS != rc && PMIX_SUCCESS == ret) {
        ret = rc;
    }

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
    pmix_list_remove_item(&prte_grpcomm_globals.fence_ops, &coll->super);
    PMIX_RELEASE(coll);
    PMIX_RELEASE(sig);
}

static prte_grpcomm_fence_t* get_tracker(prte_grpcomm_fence_signature_t *sig, bool create)
{
    prte_grpcomm_fence_t *coll;
    int rc;

    /* search the existing tracker list to see if this already exists */
    PMIX_LIST_FOREACH(coll, &prte_grpcomm_globals.fence_ops, prte_grpcomm_fence_t) {
        if (sig->sz == coll->sig->sz) {
            // must match proc signature
            if (0 == memcmp(sig->signature, coll->sig->signature, sig->sz * sizeof(pmix_proc_t))) {
                PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_globals.output,
                                     "%s grpcomm:base:returning existing collective",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                return coll;
            }
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
                                                            bool create)
{
    return get_tracker(sig, create);
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
