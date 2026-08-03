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

#include "grpcomm_direct.h"
#include "src/mca/grpcomm/base/base.h"

/* internal functions */
static void fence(int sd, short args, void *cbdata);
static prte_grpcomm_fence_t* get_tracker(prte_grpcomm_direct_fence_signature_t *sig, bool create);
static int create_dmns(prte_grpcomm_direct_fence_signature_t *sig,
                       pmix_rank_t **dmns, size_t *ndmns);
static int fence_sig_pack(pmix_data_buffer_t *bkt,
                          prte_grpcomm_direct_fence_signature_t *sig);
static int fence_sig_unpack(pmix_data_buffer_t *buffer,
                            prte_grpcomm_direct_fence_signature_t **sig);
static void check_complete(prte_grpcomm_fence_t *coll);

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

int prte_grpcomm_direct_fence(const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t info[], size_t ninfo, char *data,
                              size_t ndata, pmix_modex_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_fence_caddy_t *cd;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:fence",
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
                        &prte_mca_grpcomm_direct_component.recovery_epoch, 1, PMIX_UINT32);
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
 * anything else down with it. Only the controller calls this: it broadcasts a
 * release carrying the signature and a status but no gathered data, which the
 * normal release path hands to each daemon's local participants. */
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
    (void) prte_grpcomm.xcast(PRTE_RML_TAG_FENCE_RELEASE, reply);
    PMIX_DATA_BUFFER_RELEASE(reply);
    /* the tracker goes when that release comes back around */
    coll->aborting = true;
}

void prte_grpcomm_direct_fence_restart(void)
{
    prte_grpcomm_fence_t *coll, *nxt;
    pmix_data_buffer_t *framed;
    int rc;

    PMIX_LIST_FOREACH_SAFE(coll, nxt, &prte_mca_grpcomm_direct_component.fence_ops,
                           prte_grpcomm_fence_t) {
        if (coll->aborting) {
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

        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct:fence restarting at epoch %u, "
                             "nexpected now %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             (unsigned) prte_mca_grpcomm_direct_component.recovery_epoch,
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
void prte_grpcomm_direct_fence_fault_handler(const prte_rml_recovery_status_t* status)
{
    prte_grpcomm_fence_t *coll, *nxt;

    if (0 == pmix_list_get_size(&prte_mca_grpcomm_direct_component.fence_ops)) {
        return;
    }

    if (PRTE_RML_FAULT_SCOPE_GLOBAL != status->scope) {
        /* A revival arrives here: local scope with no failed ranks, since a
         * death's local pass returns early when it has nothing new. A revival
         * reshapes the tree as a death does, but it rides a forward-first
         * broadcast, so it cannot give the parent-before-child ordering a
         * restart depends on - end the fences instead. */
        if (PRTE_PROC_IS_MASTER && 0 == status->failed_ranks.size) {
            PMIX_LIST_FOREACH_SAFE(coll, nxt, &prte_mca_grpcomm_direct_component.fence_ops,
                                   prte_grpcomm_fence_t) {
                if (!coll->aborting) {
                    abort_fence_op(coll, PMIX_ERR_LOST_CONNECTION);
                }
            }
        }
        return;
    }

    if (!PRTE_PROC_IS_MASTER) {
        return;
    }
    PMIX_LIST_FOREACH_SAFE(coll, nxt, &prte_mca_grpcomm_direct_component.fence_ops,
                           prte_grpcomm_fence_t) {
        if (coll->aborting) {
            continue;
        }
        if (!prte_grpcomm_direct_procs_lost(coll->sig->signature, coll->sig->sz)) {
            /* only the paths between us changed - the restart driven by the
             * component's epoch advance re-converges this one */
            continue;
        }
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct:fence ending a fence that lost a "
                             "participant to a failed daemon",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        abort_fence_op(coll, PMIX_ERR_LOST_CONNECTION);
    }
}

static void fence(int sd, short args, void *cbdata)
{
    prte_pmix_fence_caddy_t *cd = (prte_pmix_fence_caddy_t *) cbdata;
    prte_grpcomm_direct_fence_signature_t sig;
    prte_grpcomm_fence_t *coll = NULL;
    int rc;
    /* what our own participants are told if this contribution never makes
     * it into the rollup - PMIX_SUCCESS means it did */
    pmix_status_t st = PMIX_SUCCESS;
    pmix_data_buffer_t *relay, *framed, bkt;
    pmix_byte_object_t bo;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(cd);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: fence",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    /* compute the signature of this collective */
    PMIX_CONSTRUCT(&sig, prte_grpcomm_direct_fence_signature_t);
    sig.sz = cd->nprocs;
    sig.signature = (pmix_proc_t *) malloc(sig.sz * sizeof(pmix_proc_t));
    memcpy(sig.signature, cd->procs, sig.sz * sizeof(pmix_proc_t));

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

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: fence",
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
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:fence sending to ourself",
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

void prte_grpcomm_direct_fence_recv(int status, pmix_proc_t *sender,
                                    pmix_data_buffer_t *buffer,
                                    prte_rml_tag_t tag, void *cbdata)
{
    int32_t cnt;
    int rc, timeout, slot;
    uint32_t stamp;
    size_t n, ninfo;
    pmix_status_t st;
    pmix_info_t *info = NULL;
    prte_grpcomm_direct_fence_signature_t *sig = NULL;
    prte_grpcomm_fence_t *coll;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct fence recvd from %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(sender)));

    /* every message on this tag opens with the sender's recovery epoch */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &stamp, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }
    if (stamp < prte_mca_grpcomm_direct_component.recovery_epoch) {
        /* sent before a failure this daemon has already recovered from, so it
         * belongs to a round that no longer exists */
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct fence stale epoch %u (at %u) - dropping",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned) stamp,
                             (unsigned) prte_mca_grpcomm_direct_component.recovery_epoch));
        return;
    }
    if (stamp > prte_mca_grpcomm_direct_component.recovery_epoch) {
        /* should be unreachable - see the recovery_epoch commentary in
         * grpcomm_direct.h - but adopting it is what our own fault notice
         * would do a moment later, so do that rather than lose the message */
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_ORDER_MSG);
        prte_grpcomm_direct_advance_epoch(stamp);
    }

    /* unpack the signature */
    rc = fence_sig_unpack(buffer, &sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        /* sig is NULL on failure - bail rather than deref it in get_tracker */
        return;
    }

    /* check for the tracker and create it if not found */
    if (NULL == (coll = get_tracker(sig, true))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PMIX_RELEASE(sig);
        return;
    }
    PMIX_RELEASE(sig);

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

    /* transfer any data */
    rc = PMIx_Data_copy_payload(&coll->bucket, buffer);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
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

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct fence recv nexpected %d nrep %d",
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
    pmix_data_buffer_t *reply, *framed;
    pmix_info_t *info = NULL;
    size_t ninfo = 0;
    int rc;

    if (coll->converged || coll->aborting) {
        return;
    }
    if (coll->nreported < coll->nexpected) {
        return;
    }
    coll->converged = true;

    if (PRTE_PROC_IS_MASTER) {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct fence HNP reports complete",
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
        (void) prte_grpcomm.xcast(PRTE_RML_TAG_FENCE_RELEASE, reply);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct fence rollup complete - sending to %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PRTE_NAME_PRINT(PRTE_PROC_MY_PARENT)));

    PMIX_DATA_BUFFER_CREATE(reply);
    rc = fence_sig_pack(reply, coll->sig);
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

static void relcb(void *cbdata)
{
    uint8_t *data = (uint8_t *) cbdata;

    if (NULL != data) {
        free(data);
    }
}

void prte_grpcomm_direct_fence_release(int status, pmix_proc_t *sender,
                                       pmix_data_buffer_t *buffer,
                                       prte_rml_tag_t tag, void *cbdata)
{
    int32_t cnt;
    int rc, ret;
    prte_grpcomm_direct_fence_signature_t *sig = NULL;
    prte_grpcomm_fence_t *coll;
    pmix_byte_object_t bo;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct: fence release called with %d bytes",
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
    pmix_list_remove_item(&prte_mca_grpcomm_direct_component.fence_ops, &coll->super);
    PMIX_RELEASE(coll);
    PMIX_RELEASE(sig);
}

static prte_grpcomm_fence_t* get_tracker(prte_grpcomm_direct_fence_signature_t *sig, bool create)
{
    prte_grpcomm_fence_t *coll;
    int rc;

    /* search the existing tracker list to see if this already exists */
    PMIX_LIST_FOREACH(coll, &prte_mca_grpcomm_direct_component.fence_ops, prte_grpcomm_fence_t) {
        if (sig->sz == coll->sig->sz) {
            // must match proc signature
            if (0 == memcmp(sig->signature, coll->sig->signature, sig->sz * sizeof(pmix_proc_t))) {
                PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                     "%s grpcomm:base:returning existing collective",
                                     PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
                return coll;
            }
        }
    }
    /* if we get here, then this is a new collective - so create
     * the tracker for it */
    if (!create) {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:base: not creating new coll",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

        return NULL;
    }
    coll = PMIX_NEW(prte_grpcomm_fence_t);
    // we have to know the participating procs
    coll->sig = PMIX_NEW(prte_grpcomm_direct_fence_signature_t);
    coll->sig->sz = sig->sz;
    if (0 < coll->sig->sz) {
        coll->sig->signature = (pmix_proc_t *) malloc(coll->sig->sz * sizeof(pmix_proc_t));
        memcpy(coll->sig->signature, sig->signature, coll->sig->sz * sizeof(pmix_proc_t));
    }
    pmix_list_append(&prte_mca_grpcomm_direct_component.fence_ops, &coll->super);

    /* now get the daemons involved */
    if (PRTE_SUCCESS != (rc = create_dmns(sig, &coll->dmns, &coll->ndmns))) {
        PRTE_ERROR_LOG(rc);
        /* a tracker with no daemon set can never be completed, and leaving it
         * on the list is worse than losing it: the next fence of the same
         * signature would find this one, see a rollup that expects nothing,
         * and answer with data it never gathered */
        pmix_list_remove_item(&prte_mca_grpcomm_direct_component.fence_ops, &coll->super);
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
prte_grpcomm_fence_t *prte_grpcomm_direct_fence_get_tracker(prte_grpcomm_direct_fence_signature_t *sig,
                                                            bool create)
{
    return get_tracker(sig, create);
}

static int create_dmns(prte_grpcomm_direct_fence_signature_t *sig,
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

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:fence:create_dmns called with %s signature size %" PRIsize_t "",
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
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:fence::create_dmns called for all procs in job %s",
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
                    PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                         "%s grpcomm:direct:fence::create_dmns adding daemon %s to list",
                                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                         PRTE_NAME_PRINT(&node->daemon->name)));
                    nm = PMIX_NEW(prte_namelist_t);
                    PMIX_LOAD_PROCID(&nm->name, PRTE_PROC_MY_NAME->nspace, node->daemon->name.rank);
                    pmix_list_append(&ds, &nm->super);
                }
            }
        } else {
            /* lookup the daemon for this proc and add it to the list */
            PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
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
            PMIX_OUTPUT_VERBOSE((5, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:fence::create_dmns adding daemon %s to array",
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
                          prte_grpcomm_direct_fence_signature_t *sig)
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
                            prte_grpcomm_direct_fence_signature_t **sig)
{
    pmix_status_t rc;
    int32_t cnt;
    prte_grpcomm_direct_fence_signature_t *s;

    s = PMIX_NEW(prte_grpcomm_direct_fence_signature_t);

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
