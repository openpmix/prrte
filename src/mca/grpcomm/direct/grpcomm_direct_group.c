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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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
#include "src/mca/rmaps/rmaps_types.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/util/name_fns.h"
#include "src/util/nidmap.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"

#include "grpcomm_direct.h"
#include "src/mca/grpcomm/base/base.h"

static void group(int sd, short args, void *cbdata);
static void check_complete(prte_grpcomm_group_t *coll);
static void group_timeout(int sd, short args, void *cbdata);
static bool group_op_completed(prte_grpcomm_direct_group_signature_t *sig);
static void group_op_forget(prte_grpcomm_direct_group_signature_t *sig);
#if PRTE_PMIX_HAVE_GROUP_FT
static void collect_departed(prte_grpcomm_group_t *coll);
#endif

/* The recovery epoch lives on the component and is shared with fence: one
 * failure means one restart of every collective in flight, so there is one
 * counter. See the commentary on recovery_epoch in grpcomm_direct.h for why a
 * daemon-wide counter rather than a per-tracker one, and for the ordering
 * argument that makes a simultaneous restart possible without a protocol of
 * its own.
 */

/* Frame a contribution with the current epoch. The body is copied, not
 * consumed, so the caller keeps ownership of it. */
static int pack_epoch_frame(pmix_data_buffer_t *framed, pmix_data_buffer_t *body)
{
    pmix_status_t rc;

    rc = PMIx_Data_pack(NULL, framed, &prte_mca_grpcomm_direct_component.recovery_epoch, 1, PMIX_UINT32);
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

/* Take a copy of a proc array a directive carried into the signature.
 *
 * The array belongs to the caller: the directives come straight from the
 * PMIx server upcall, and PMIx frees them - the darray and the array inside
 * it - once the operation completes. The signature's destructor frees
 * whatever the signature holds, so pointing at the caller's array would have
 * us free it out from under PMIx and have PMIx free it a second time. Every
 * other populator of these fields (the tracker cache in get_tracker(), the
 * unpack of an arriving signature) already allocates, so this keeps the
 * ownership rule uniform: the signature owns its arrays, always. */
static pmix_status_t copy_directive_procs(const pmix_info_t *dir,
                                          pmix_proc_t **procs, size_t *nprocs)
{
    pmix_data_array_t *darray = dir->value.data.darray;

    if (PMIX_DATA_ARRAY != dir->value.type || NULL == darray ||
        NULL == darray->array || 0 == darray->size) {
        /* nothing usable was given - not an error, just no members */
        return PMIX_SUCCESS;
    }
    if (NULL != *procs) {
        /* a second directive of the same key - the PMIx server aggregates
         * these, so this should not happen, but do not strand the first */
        PMIX_PROC_FREE(*procs, *nprocs);
    }
    PMIX_PROC_CREATE(*procs, darray->size);
    if (NULL == *procs) {
        *nprocs = 0;
        return PMIX_ERR_NOMEM;
    }
    memcpy(*procs, darray->array, darray->size * sizeof(pmix_proc_t));
    *nprocs = darray->size;
    return PMIX_SUCCESS;
}

/* Scan the directives a participant gave us, filling in the signature and the
 * values collected alongside it. Split out of group() so it can be exercised
 * without a DVM - it reads nothing but its arguments. */
pmix_status_t prte_grpcomm_direct_group_parse_directives(prte_grpcomm_direct_group_signature_t *sig,
                                                         const pmix_info_t *directives, size_t ndirs,
                                                         int *timeout, pmix_status_t *st,
                                                         void *grpinfo, void *endpts)
{
    pmix_status_t rc;
    size_t i;

    for (i = 0; i < ndirs; i++) {
        /* see if they want a context id assigned */
        if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ASSIGN_CONTEXT_ID)) {
            sig->assignID = PMIX_INFO_TRUE(&directives[i]);

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_BOOTSTRAP)) {
            PMIX_VALUE_GET_NUMBER(rc, &directives[i].value, sig->bootstrap, size_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_LOCAL_COLLECTIVE_STATUS)) {
            PMIX_VALUE_GET_NUMBER(rc, &directives[i].value, *st, pmix_status_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_TIMEOUT)) {
            PMIX_VALUE_GET_NUMBER(rc, &directives[i].value, *timeout, int);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_ADD_MEMBERS)) {
            // there is only one of these as it is aggregated by the
            // PMIx server library
            rc = copy_directive_procs(&directives[i], &sig->addmembers, &sig->naddmembers);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_INFO)) {
            rc = PMIx_Info_list_xfer(grpinfo, &directives[i]);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_PROC_DATA)) {
            rc = PMIx_Info_list_xfer(endpts, &directives[i]);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
            }

        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_FINAL_MEMBERSHIP_ORDER)) {
            rc = copy_directive_procs(&directives[i], &sig->final_order, &sig->nfinal);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                return rc;
            }
#if PRTE_PMIX_HAVE_GROUP_FT
        } else if (PMIX_CHECK_KEY(&directives[i], PMIX_GROUP_FT_COLLECTIVE)) {
            sig->ft_collective = PMIX_INFO_TRUE(&directives[i]);
#endif
        }
    }

    return PMIX_SUCCESS;
}

static prte_grpcomm_group_t *get_tracker(prte_grpcomm_direct_group_signature_t *sig, bool create);

static int create_dmns(prte_grpcomm_direct_group_signature_t *sig,
                       pmix_rank_t **dmns, size_t *ndmns);

static int pack_signature(pmix_data_buffer_t *buf,
                          prte_grpcomm_direct_group_signature_t *sig);

static int unpack_signature(pmix_data_buffer_t *buf,
                            prte_grpcomm_direct_group_signature_t **sig);

int prte_grpcomm_direct_group(pmix_group_operation_t op, char *grpid,
                              const pmix_proc_t procs[], size_t nprocs,
                              const pmix_info_t directives[], size_t ndirs,
                              pmix_info_cbfunc_t cbfunc, void *cbdata)
{
    prte_pmix_grp_caddy_t *cd;

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:group %s for \"%s\" with %lu procs",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         PMIx_Group_operation_string(op), grpid, nprocs));

    cd = PMIX_NEW(prte_pmix_grp_caddy_t);
    cd->op = op;
    cd->grpid = strdup(grpid);
    cd->procs = procs;
    cd->nprocs = nprocs;
    cd->directives = directives;
    cd->ndirs = ndirs;
    cd->cbfunc = cbfunc;
    cd->cbdata = cbdata;

    /* must push this into the event library to ensure we can
     * access framework-global data safely */
    prte_event_set(prte_event_base, &cd->ev, -1, PRTE_EV_WRITE, group, cd);
    PMIX_POST_OBJECT(cd);
    prte_event_active(&cd->ev, PRTE_EV_WRITE, 1);
    return PRTE_SUCCESS;
}

/* Abort an in-flight group collective cleanly instead of tearing down the DVM.
 * Only the HNP calls this - it is the master of every group op (rollups
 * converge there) and the sole xcast source. It broadcasts a GROUP_RELEASE
 * carrying just the signature and a completion status; that drives each
 * daemon's prte_grpcomm_direct_grp_release to complete its local participants
 * with that status and delete the tracker (see the PMIX_SUCCESS != st path
 * there). No membership/grpinfo/endpts payload is needed: the release reader
 * skips all of that for a non-success construct, and a destruct never reads it. */
static void abort_group_op(prte_grpcomm_group_t *coll, pmix_status_t st)
{
    pmix_data_buffer_t *reply;
    pmix_status_t rc;

    PMIX_DATA_BUFFER_CREATE(reply);
    /* pack the signature */
    rc = pack_signature(reply, coll->sig);
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    /* pack the completion status */
    rc = PMIx_Data_pack(NULL, reply, &st, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(reply);
        return;
    }
    /* send the release via xcast to all daemons (ourselves included).
     * xcast copies the payload, so the buffer is still ours to free */
    (void) prte_grpcomm.xcast(PRTE_RML_TAG_GROUP_RELEASE, reply);
    PMIX_DATA_BUFFER_RELEASE(reply);
    /* the tracker is not deleted here - it goes when the release we just
     * broadcast comes back around - so mark it, both to keep the rollup from
     * being driven any further and to keep a second abort from broadcasting */
    coll->aborting = true;
}

/* The controller's guard timer for a collective a participant put a deadline
 * on. Firing it completes every participant with PMIX_ERR_TIMEOUT rather than
 * leaving them blocked in PMIx_Group_construct forever. */
static void group_timeout(int sd, short args, void *cbdata)
{
    prte_grpcomm_group_t *coll = (prte_grpcomm_group_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(coll);
    coll->tev_active = false;
    if (coll->converged || coll->aborting) {
        return;
    }
    pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                        "%s grpcomm:direct:group timeout on \"%s\" after %d seconds",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), coll->sig->groupID,
                        coll->timeout);
    coll->status = PMIX_ERR_TIMEOUT;
    abort_group_op(coll, PMIX_ERR_TIMEOUT);
}

#if PRTE_PMIX_HAVE_GROUP_FT
/* Locate the in-flight construct tracker for a group by name. Used when a
 * cancel arrives: the cancel carries op == PMIX_GROUP_CANCEL, which by design
 * does not match the construct tracker's op, so we look it up by groupID. */
static prte_grpcomm_group_t* find_construct_op(const char *groupID)
{
    prte_grpcomm_group_t *coll;

    PMIX_LIST_FOREACH(coll, &prte_mca_grpcomm_direct_component.group_ops, prte_grpcomm_group_t) {
        if (PMIX_GROUP_CONSTRUCT == coll->sig->op &&
            0 == strcmp(groupID, coll->sig->groupID)) {
            return coll;
        }
    }
    return NULL;
}

/* Route a group-cancel request to the HNP. Called on the daemon whose PMIx
 * server requested the cancel (e.g. its client hit the construct timeout). We
 * carry just a signature (op == PMIX_GROUP_CANCEL + groupID) up to the HNP,
 * which locates the in-flight construct and aborts it (see the CANCEL branch in
 * prte_grpcomm_direct_grp_recv). That abort completes every participant's
 * PMIx_Group_construct with PMIX_GROUP_CONSTRUCT_ABORT via the normal release
 * path, so the cancel itself is simply acknowledged once dispatched. */
static void request_group_cancel(prte_pmix_grp_caddy_t *cd)
{
    prte_grpcomm_direct_group_signature_t sig;
    pmix_data_buffer_t *relay;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&sig, prte_grpcomm_direct_group_signature_t);
    sig.op = PMIX_GROUP_CANCEL;
    sig.groupID = strdup(cd->grpid);

    PMIX_DATA_BUFFER_CREATE(relay);
    /* every message on the GROUP tag opens with the epoch, so the receiver
     * can read it before it knows what kind of message this is - a cancel
     * carries one even though nothing filters it against the epoch */
    rc = PMIx_Data_pack(NULL, relay, &prte_mca_grpcomm_direct_component.recovery_epoch, 1, PMIX_UINT32);
    if (PMIX_SUCCESS == rc) {
        /* note this one answers in PRTE codes, and we acknowledge in PMIx
         * statuses - the two agree only on success */
        rc = pack_signature(relay, &sig);
        if (PRTE_SUCCESS != rc) {
            rc = prte_pmix_convert_rc(rc);
        }
    }
    PMIX_DESTRUCT(&sig);
    if (PMIX_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        goto ack;
    }
    PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, relay, PRTE_RML_TAG_GROUP);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        rc = prte_pmix_convert_rc(rc);
        goto ack;
    }
    /* the cancel has been dispatched - acknowledge success. The construct abort
     * is delivered separately through the construct's own release. */
    rc = PMIX_SUCCESS;

ack:
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, NULL, 0, cd->cbdata, NULL, NULL);
    }
}
#endif /* PRTE_PMIX_HAVE_GROUP_FT */

/* Is any daemon hosting a member of this operation known to have failed? Keyed
 * on the membership rather than on the resolved daemon set, because the
 * membership is stable and present on every daemon while the daemon set is
 * derived state that a torn-down node can make unresolvable. */
static bool group_lost_member(prte_grpcomm_group_t *coll)
{
    return prte_grpcomm_direct_procs_lost(coll->sig->members, coll->sig->nmembers)
           || prte_grpcomm_direct_procs_lost(coll->sig->addmembers, coll->sig->naddmembers);
}

/* Is this entry one of the members we have determined to be lost? Used to keep
 * them out of the final membership. A wildcard entry never matches: it names a
 * whole namespace, so it stays in the membership even when some of that
 * namespace has gone, and expanding it here would change what the caller
 * asked for. */
static bool member_is_departed(prte_grpcomm_group_t *coll, const pmix_proc_t *member)
{
    size_t n;

    if (0 == coll->ndeparted || PMIX_RANK_WILDCARD == member->rank) {
        return false;
    }
    for (n = 0; n < coll->ndeparted; n++) {
        if (PMIX_CHECK_PROCID(member, &coll->departed[n])) {
            return true;
        }
    }
    return false;
}

/* Restart every in-flight group rollup at the epoch the component has just
 * moved to. The caller is prte_grpcomm_direct_advance_epoch(), which is what
 * guarantees every daemon does this on the same broadcast - a partial restart,
 * where one daemon re-sends into a parent that did not reset, would
 * double-count that subtree and complete the collective with another one
 * missing. */
void prte_grpcomm_direct_group_restart(void)
{
    prte_grpcomm_group_t *coll, *nxt;
    pmix_data_buffer_t *framed;
    size_t n;
    int rc;

    PMIX_LIST_FOREACH_SAFE(coll, nxt, &prte_mca_grpcomm_direct_component.group_ops,
                           prte_grpcomm_group_t) {
        /* a bootstrap has no rollup to restart, and an operation already being
         * torn down by an abort is not going to converge either way */
        if (coll->bootstrap || coll->aborting) {
            continue;
        }

        /* discard everything gathered under the old tree */
        pmix_bitmap_clear_all_bits(&coll->reported_slots);
        coll->self_reported = false;
        coll->nreported = 0;
        coll->converged = false;
        coll->status = PMIX_SUCCESS;
        PMIx_Info_list_release(coll->grpinfo);
        PMIx_Info_list_release(coll->endpts);
        coll->grpinfo = PMIx_Info_list_start();
        coll->endpts = PMIx_Info_list_start();

        /* recompute what the repaired tree owes us. dmns deliberately stays
         * the full pre-fault set - it is the record of who was supposed to
         * take part, and prte_rml_get_num_contributors() already skips the
         * daemons now known to have failed */
        coll->nexpected = prte_rml_get_num_contributors(coll->dmns, coll->ndmns);
        for (n = 0; n < coll->ndmns; n++) {
            if (coll->dmns[n] == PRTE_PROC_MY_NAME->rank) {
                coll->nexpected++;
                break;
            }
        }

        pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                            "%s grpcomm:direct:group restarting \"%s\" at epoch %u, "
                            "nexpected now %d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), coll->sig->groupID,
                            (unsigned) prte_mca_grpcomm_direct_component.recovery_epoch, (int) coll->nexpected);

        if (NULL == coll->my_contribution) {
            /* nothing of our own to re-offer - we are relaying for our
             * subtree. If that subtree just went away, nexpected is now zero
             * and this completes the operation on the spot, which is what
             * keeps the loss of a pure relay from stalling the collective */
            check_complete(coll);
            continue;
        }

        /* re-offer our own contribution at the new epoch. Sending to ourselves
         * defers to the event loop, so we never re-enter grp_recv while
         * walking the tracker list */
        PMIX_DATA_BUFFER_CREATE(framed);
        rc = pack_epoch_frame(framed, coll->my_contribution);
        if (PRTE_SUCCESS != rc) {
            PMIX_DATA_BUFFER_RELEASE(framed);
            continue;
        }
        PRTE_RML_SEND(rc, PRTE_PROC_MY_NAME->rank, framed, PRTE_RML_TAG_GROUP);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(framed);
        }
    }
}

void prte_grpcomm_direct_group_fault_handler(const prte_rml_recovery_status_t* status)
{
    prte_grpcomm_group_t *coll, *nxt;

    if (0 == pmix_list_get_size(&prte_mca_grpcomm_direct_component.group_ops)) {
        return;
    }

    if (PRTE_RML_FAULT_SCOPE_GLOBAL != status->scope) {
        /* A revival reaches us here: it leaves the scope at its local default
         * and carries no failed ranks, whereas a death's local pass returns
         * early when it has nothing new to report, so an empty rank list is an
         * unambiguous discriminator.
         *
         * A revival reshapes the tree and invalidates the expected counts just
         * as a death does, but it cannot be recovered the same way: it is
         * driven by DAEMON_REVIVED, which is deliberately forward-first in the
         * xcast, so a daemon can forward the notice to a child before acting on
         * it and the simultaneous restart an epoch advance depends on is not
         * available. Abort instead. Today this path does nothing at all, which
         * means a group operation in flight across an unheal simply hangs, so
         * failing it cleanly is the better of the two. */
        if (PRTE_PROC_IS_MASTER && 0 == status->failed_ranks.size) {
            PMIX_LIST_FOREACH_SAFE(coll, nxt, &prte_mca_grpcomm_direct_component.group_ops,
                                   prte_grpcomm_group_t) {
                if (coll->aborting) {
                    continue;
                }
                pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                                    "%s grpcomm:direct:group aborting \"%s\" - a daemon "
                                    "returned and reshaped the tree",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), coll->sig->groupID);
                abort_group_op(coll,
                               (PMIX_GROUP_CONSTRUCT == coll->sig->op)
                                   ? PMIX_GROUP_CONSTRUCT_ABORT : PMIX_SUCCESS);
            }
        }
        return;
    }

    /* Global scope: the tree is repaired, and this notice reached every daemon
     * from a single broadcast in the same order, which is what lets all of
     * them restart their rollups together. */
    if (PRTE_PROC_IS_MASTER) {
        /* Fail fast the operations that provably cannot survive, so their
         * participants are not made to wait out a re-convergence whose only
         * possible outcome is the abort below. This is an optimization: the
         * controller applies the same policy when the rollup completes, which
         * is what covers an operation it holds no tracker for yet. */
        PMIX_LIST_FOREACH_SAFE(coll, nxt, &prte_mca_grpcomm_direct_component.group_ops,
                               prte_grpcomm_group_t) {
            if (coll->aborting) {
                continue;
            }
            if (coll->bootstrap) {
                /* A bootstrap has no resolved daemon set to test, and its
                 * leader count is a number with no identities attached, so
                 * there is no way to work out how much of it died. */
                abort_group_op(coll,
                               (PMIX_GROUP_CONSTRUCT == coll->sig->op)
                                   ? PMIX_GROUP_CONSTRUCT_ABORT : PMIX_SUCCESS);
                continue;
            }
            if (PMIX_GROUP_CONSTRUCT == coll->sig->op &&
                !coll->sig->ft_collective &&
                group_lost_member(coll)) {
                pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                                    "%s grpcomm:direct:group aborting \"%s\" - a member's "
                                    "daemon failed and the group is not fault tolerant",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), coll->sig->groupID);
                abort_group_op(coll, PMIX_GROUP_CONSTRUCT_ABORT);
            }
        }
    }

}


static void group(int sd, short args, void *cbdata)
{
    prte_pmix_grp_caddy_t *cd = (prte_pmix_grp_caddy_t*)cbdata;
    prte_grpcomm_direct_group_signature_t sig;
    prte_grpcomm_group_t *coll;
    pmix_data_buffer_t *relay, *framed;
    pmix_status_t rc, st = PMIX_SUCCESS;
    int timeout = 0;
    void *endpts, *grpinfo;
    pmix_data_array_t darray;
    pmix_info_t *info;
    size_t ninfo;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

#if PRTE_PMIX_HAVE_GROUP_FT
    /* a cancel is not a rollup collective - route it to the HNP to abort the
     * in-flight construct, then we are done */
    if (PMIX_GROUP_CANCEL == cd->op) {
        request_group_cancel(cd);
        PMIX_RELEASE(cd);
        return;
    }
#endif

    /* compute the signature of this collective */
    PMIX_CONSTRUCT(&sig, prte_grpcomm_direct_group_signature_t);
    sig.groupID = strdup(cd->grpid);
    if (NULL != cd->procs) {
        sig.nmembers = cd->nprocs;
        PMIX_PROC_CREATE(sig.members, sig.nmembers);
        memcpy(sig.members, cd->procs, sig.nmembers * sizeof(pmix_proc_t));
    } else {
        // if no procs were given, then this must be a follower
        sig.follower = true;
    }
    sig.op = cd->op;

    // setup to track endpts and grpinfo
    endpts = PMIx_Info_list_start();
    grpinfo = PMIx_Info_list_start();

    /* check the directives */
    rc = prte_grpcomm_direct_group_parse_directives(&sig, cd->directives, cd->ndirs,
                                                    &timeout, &st, grpinfo, endpts);
    if (PMIX_SUCCESS != rc) {
        PMIX_DESTRUCT(&sig);
        goto error;
    }

    /* a local client is starting this operation, which is proof that any
     * earlier operation of the same name and type is finished with - drop it
     * from the completed memo so this one is not mistaken for a straggler */
    group_op_forget(&sig);

    /* create a tracker for this operation */
    if (NULL == (coll = get_tracker(&sig, true))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PMIX_RELEASE(cd);
        PMIX_DESTRUCT(&sig);
        return;
    }
    coll->cbfunc = cd->cbfunc;
    coll->cbdata = cd->cbdata;

    // create the relay buffer
    PMIX_DATA_BUFFER_CREATE(relay);

    /* pack the signature */
    rc = pack_signature(relay, &sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        PMIX_DESTRUCT(&sig);
        /* the error label answers our caller, which reads PMIx statuses -
         * and this one is a PRTE code */
        rc = prte_pmix_convert_rc(rc);
        goto error;
    }

    // pack the local collective status
    rc = PMIx_Data_pack(NULL, relay, &st, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        PMIX_DESTRUCT(&sig);
        goto error;
    }

    // pack any timeout directive
    rc = PMIx_Data_pack(NULL, relay, &timeout, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(relay);
        PMIX_DESTRUCT(&sig);
        goto error;
    }

    if (PMIX_GROUP_CONSTRUCT == sig.op) {
        // pack any group info
        PMIx_Info_list_convert(grpinfo, &darray);
        info = (pmix_info_t*)darray.array;
        ninfo = darray.size;
        rc = PMIx_Data_pack(NULL, relay, &ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(relay);
            PMIX_DESTRUCT(&sig);
            goto error;
        }
        if (0 < ninfo) {
            rc = PMIx_Data_pack(NULL, relay, info, ninfo, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(relay);
                PMIX_DESTRUCT(&sig);
                goto error;
            }
        }
        PMIX_DATA_ARRAY_DESTRUCT(&darray);

        // pack any endpts
        PMIx_Info_list_convert(endpts, &darray);
        info = (pmix_info_t*)darray.array;
        ninfo = darray.size;
        rc = PMIx_Data_pack(NULL, relay, &ninfo, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(relay);
            PMIX_DESTRUCT(&sig);
            goto error;
        }
        if (0 < ninfo) {
            rc = PMIx_Data_pack(NULL, relay, info, ninfo, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(relay);
                PMIX_DESTRUCT(&sig);
                goto error;
            }
        }
        PMIX_DATA_ARRAY_DESTRUCT(&darray);
    }
    PMIx_Info_list_release(grpinfo);
    PMIx_Info_list_release(endpts);
    /* consumed - NULL them so the error label below does not double-release
     * if a send fails past this point */
    grpinfo = NULL;
    endpts = NULL;

    /* Keep our own contribution so a fault can replay it. Recovery resets
     * every tracker and has each daemon re-inject what it originally
     * contributed; without a copy here there would be nothing to re-inject,
     * since PRTE_RML_SEND takes ownership of the buffer we hand it. */
    /* a daemon can enter here more than once for the same tracker - several
     * bootstrap leaders, or a leader and a follower - so let go of any
     * earlier copy rather than stranding it */
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
        PMIX_DESTRUCT(&sig);
        goto error;
    }

    /* stamp it with the current epoch and send that */
    PMIX_DATA_BUFFER_CREATE(framed);
    rc = pack_epoch_frame(framed, relay);
    PMIX_DATA_BUFFER_RELEASE(relay);
    if (PRTE_SUCCESS != rc) {
        PMIX_DATA_BUFFER_RELEASE(framed);
        PMIX_DESTRUCT(&sig);
        rc = prte_pmix_convert_rc(rc);
        goto error;
    }
    relay = framed;

    /* if this is a bootstrap operation, send it directly to the HNP */
    if (coll->bootstrap) {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct:grp bootstrap sending %lu bytes to HNP",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), relay->bytes_used));

        PRTE_RML_SEND(rc, PRTE_PROC_MY_HNP->rank, relay,
                      PRTE_RML_TAG_GROUP);
        if (PRTE_SUCCESS != rc) {
            PMIX_DATA_BUFFER_RELEASE(relay);
            rc = prte_pmix_convert_rc(rc);
            PMIX_DESTRUCT(&sig);
            goto error;
        }
        PMIX_DESTRUCT(&sig);
        PMIX_RELEASE(cd);
        return;
    }
    PMIX_DESTRUCT(&sig);

    /* send this to ourselves for processing */
    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:grp sending to ourself",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    PRTE_RML_SEND(rc, PRTE_PROC_MY_NAME->rank, relay,
                  PRTE_RML_TAG_GROUP);
    if (PRTE_SUCCESS != rc) {
        PMIX_DATA_BUFFER_RELEASE(relay);
        rc = prte_pmix_convert_rc(rc);
        goto error;
    }
    PMIX_RELEASE(cd);
    return;

error:
    /* the grpinfo/endpts trackers are released (and NULLed) on the success
     * path above; on any error jump to here before that point they are still
     * open, so release them now.  The NULL guard makes this safe for the
     * post-release send-failure jumps too. */
    if (NULL != grpinfo) {
        PMIx_Info_list_release(grpinfo);
    }
    if (NULL != endpts) {
        PMIx_Info_list_release(endpts);
    }
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, NULL, 0, cd->cbdata, NULL, NULL);
    }
    PMIX_RELEASE(cd);
}

void prte_grpcomm_direct_grp_recv(int status, pmix_proc_t *sender,
                                  pmix_data_buffer_t *buffer,
                                  prte_rml_tag_t tag, void *cbdata)
{
    int32_t cnt;
    int rc, timeout, slot;
    uint32_t stamp;
    struct timeval tv;
    size_t n, nendpts, ngrpinfo;
    pmix_status_t st;
    pmix_info_t *endpts, *grpinfo = NULL;
    prte_grpcomm_direct_group_signature_t *sig = NULL;
    prte_grpcomm_group_t *coll;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct grp recvd from %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(sender)));

    // empty buffer indicates lost connection
    if (NULL == buffer || NULL == buffer->unpack_ptr) {
        PMIX_ERROR_LOG(PMIX_ERR_EMPTY);
        return;
    }

    /* every message on this tag opens with the sender's recovery epoch */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &stamp, &cnt, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack the signature */
    rc = unpack_signature(buffer, &sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        /* sig is NULL on failure - bail rather than deref it below */
        return;
    }

#if PRTE_PMIX_HAVE_GROUP_FT
    /* a cancel request carries only the signature. Only the HNP acts - it is
     * the sole xcast source. Find the in-flight construct for this group and
     * abort it; the resulting release completes every participant's
     * PMIx_Group_construct with PMIX_GROUP_CONSTRUCT_ABORT. A missing tracker
     * means the construct already resolved, so there is nothing to cancel.
     * Note we must handle this before get_tracker, which would otherwise create
     * a spurious cancel tracker. */
    if (PMIX_GROUP_CANCEL == sig->op) {
        if (PRTE_PROC_IS_MASTER) {
            coll = find_construct_op(sig->groupID);
            if (NULL != coll) {
                abort_group_op(coll, PMIX_GROUP_CONSTRUCT_ABORT);
            }
        }
        PMIX_RELEASE(sig);
        return;
    }
#endif

    /* Reconcile the sender's recovery epoch with ours. Bootstrap operations
     * are exempt: they bypass the rollup tree entirely, so a tree repair does
     * not invalidate them and there is nothing to restart. */
    if (0 == sig->bootstrap && !sig->follower) {
        if (stamp < prte_mca_grpcomm_direct_component.recovery_epoch) {
            /* sent before the failure this daemon has already recovered from,
             * so it belongs to a round that no longer exists */
            pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                                "%s grpcomm:direct group recv stale epoch %u (at %u) - dropping",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                (unsigned) stamp, (unsigned) prte_mca_grpcomm_direct_component.recovery_epoch);
            PMIX_RELEASE(sig);
            return;
        }
        if (stamp > prte_mca_grpcomm_direct_component.recovery_epoch) {
            /* Should be unreachable - see the recovery_epoch commentary in grpcomm_direct.h. Adopt
             * it rather than dropping the contribution: that is precisely
             * what our own fault notice would do, just sooner. Log it so the
             * ordering assumption stays falsifiable. */
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_ORDER_MSG);
            prte_grpcomm_direct_advance_epoch(stamp);
        }
    }

    /* if we have already released this operation, then this is a straggler
     * that lost the race with the release - creating a tracker for it would
     * strand one nothing will ever complete */
    if (group_op_completed(sig)) {
        pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                            "%s grpcomm:direct group recv for completed op \"%s\" - ignoring",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), sig->groupID);
        PMIX_RELEASE(sig);
        return;
    }

    /* check for the tracker and create it if not found */
    if (NULL == (coll = get_tracker(sig, true))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        PMIX_RELEASE(sig);
        return;
    }

    /* Account for this contribution by *which* child subtree it came from
     * rather than simply counting it, and drop it whole if that subtree has
     * already been heard from. This has to happen before anything is merged
     * into the tracker: the group-info and endpoint accumulation below appends
     * with no key de-duplication, so a contribution counted twice is also a
     * payload duplicated twice.
     *
     * Bootstrap contributions are exempt - they are sent straight to the
     * controller from arbitrary daemons rather than up the routing tree, so a
     * subtree index is meaningless for them and they keep the leader/follower
     * counters. */
    slot = -1;
    if (!coll->bootstrap) {
        if (sender->rank == PRTE_PROC_MY_NAME->rank) {
            if (coll->self_reported) {
                PMIX_RELEASE(sig);
                return;
            }
        } else {
            slot = prte_rml_get_subtree_index(sender->rank);
            if (0 > slot) {
                /* not in any of our subtrees - we are not this daemon's
                 * parent, so this contribution is not ours to aggregate */
                PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
                PMIX_RELEASE(sig);
                return;
            }
            if (pmix_bitmap_is_set_bit(&coll->reported_slots, slot)) {
                PMIX_RELEASE(sig);
                return;
            }
        }
    }

    // unpack the local collective status
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &st, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(sig);
        return;
    }
    if (PMIX_SUCCESS != st &&
        PMIX_SUCCESS == coll->status) {
        coll->status = st;
    }

    // unpack any timeout directive
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &timeout, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(sig);
        return;
    }
    if (coll->timeout < timeout) {
        coll->timeout = timeout;
    }
    /* A group op has never been bounded in time: the timeout directive is
     * collected and relayed but nothing ever armed it, so an op that can no
     * longer converge hangs its participants indefinitely. Arm it here, on the
     * controller, which is the one daemon every rollup reaches. Only when a
     * participant actually asked for a timeout - with no directive there is no
     * deadline to impose, and the behavior is exactly as before. */
    if (PRTE_PROC_IS_MASTER && !coll->tev_active && 0 < coll->timeout) {
        prte_event_evtimer_set(prte_event_base, &coll->tev, group_timeout, coll);
        tv.tv_sec = coll->timeout;
        tv.tv_usec = 0;
        coll->tev_active = true;
        PMIX_POST_OBJECT(coll);
        prte_event_evtimer_add(&coll->tev, &tv);
    }


    if (PMIX_GROUP_CONSTRUCT == sig->op) {
        /* unpack the number of group infos */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &ngrpinfo, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(sig);
            return;
        }
        if (0 < ngrpinfo) {
            PMIX_INFO_CREATE(grpinfo, ngrpinfo);
            cnt = ngrpinfo;
            rc = PMIx_Data_unpack(NULL, buffer, grpinfo, &cnt, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_INFO_FREE(grpinfo, ngrpinfo);
                PMIX_RELEASE(sig);
                return;
            }
            for (n=0; n < ngrpinfo; n++) {
                rc = PMIx_Info_list_xfer(coll->grpinfo, &grpinfo[n]);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                }
            }
            PMIX_INFO_FREE(grpinfo, ngrpinfo);
        }

        /* unpack the number of endpoints */
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &nendpts, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            if (NULL != grpinfo) {
                PMIX_INFO_FREE(grpinfo, ngrpinfo);
            }
            PMIX_RELEASE(sig);
            return;
        }
        if (0 < nendpts) {
            PMIX_INFO_CREATE(endpts, nendpts);
            cnt = nendpts;
            rc = PMIx_Data_unpack(NULL, buffer, endpts, &cnt, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                if (NULL != grpinfo) {
                    PMIX_INFO_FREE(grpinfo, ngrpinfo);
                }
                PMIX_INFO_FREE(endpts, nendpts);
                PMIX_RELEASE(sig);
                return;
            }
            for (n=0; n < nendpts; n++) {
                rc = PMIx_Info_list_xfer(coll->endpts, &endpts[n]);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                }
            }
            PMIX_INFO_FREE(endpts, nendpts);
        }
    }

    /* track procs reported for collective */
    if (0 < sig->bootstrap) {
        // this came from a bootstrap leader
        coll->nleaders_reported++;
        pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct group recv leader nrep %d of %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) coll->nleaders_reported,
                             (int) coll->nleaders);
    } else if (sig->follower) {
        // came from a bootstrap follower
        coll->nfollowers_reported++;
        pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct group recv follower nrep %d of %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) coll->nfollowers_reported,
                             (int) coll->nfollowers);
    } else {
        /* the contribution is in - now commit the per-slot accounting for it.
         * Doing it here rather than at the test above means a message that
         * failed to parse cannot leave a subtree counted with none of its
         * data merged. */
        if (0 > slot) {
            coll->self_reported = true;
        } else {
            pmix_bitmap_set_bit(&coll->reported_slots, slot);
        }
        coll->nreported++;
        pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct group recv nexpected %d nrep %d",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) coll->nexpected,
                             (int) coll->nreported);
    }

    check_complete(coll);
    PMIX_RELEASE(sig);
}

/* Test whether this tracker's rollup is complete and, if so, answer it: the
 * controller broadcasts the release, everyone else rolls their aggregate up
 * to their parent. Factored out of grp_recv() because a contribution arriving
 * is no longer the only thing that can complete a collective - a fault can
 * lower the number of contributions we are waiting for, and the tracker has
 * to be re-tested against the new count with no message in hand.
 *
 * The completion test is ">=", not "==", precisely because nexpected can now
 * shrink underneath a tracker that has already counted more than it ends up
 * needing. The "converged" latch is what keeps that safe: without it a
 * straggler arriving after completion would drive a second release broadcast,
 * consuming another context id, or a second rollup to our parent. */
static void check_complete(prte_grpcomm_group_t *coll)
{
    int rc;
    size_t m, n, ninfo, nfinal = 0;
    pmix_proc_t *finalmembership = NULL;
    bool found;
    pmix_list_t nmlist;
    prte_namelist_t *nm;
    pmix_data_array_t darray;
    pmix_info_t *info = NULL;
    pmix_data_buffer_t *reply, *framed;

    /* an op we have already answered, or one an abort is tearing down, must
     * not be driven again */
    if (coll->converged || coll->aborting) {
        return;
    }

    /* see if everyone has reported */
    if (!((coll->bootstrap && (coll->nleaders_reported >= coll->nleaders &&
                               coll->nfollowers_reported >= coll->nfollowers)) ||
          (!coll->bootstrap && coll->nreported >= coll->nexpected))) {
        return;
    }
    coll->converged = true;
    /* the collective resolved, so the guard timer has done its job */
    if (coll->tev_active) {
        prte_event_del(&coll->tev);
        coll->tev_active = false;
    }

    if (PRTE_PROC_IS_MASTER) {
        pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct group HNP reports complete for %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), coll->sig->groupID);

        /* the allgather is complete - send the xcast */
        if (PMIX_GROUP_CONSTRUCT == coll->sig->op) {
            /* Decide what a construct that lost a member should do. This
             * is the one place with the whole picture, it runs exactly
             * once, and unlike the fault handler it also covers an
             * operation the controller held no tracker for when the
             * failure landed. */
            if (group_lost_member(coll)) {
#if PRTE_PMIX_HAVE_GROUP_FT
                if (!coll->sig->ft_collective) {
                    coll->status = PMIX_GROUP_CONSTRUCT_ABORT;
                    goto answer;
                }
                collect_departed(coll);
                pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                                    "%s grpcomm:direct:group \"%s\" completing on the "
                                    "survivors - %d member(s) lost",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    coll->sig->groupID, (int) coll->ndeparted);
#else
                /* built against a PMIx with no notion of a fault-tolerant
                 * group collective, so there is no way to have asked for
                 * one - the loss of a member ends the construct */
                coll->status = PMIX_GROUP_CONSTRUCT_ABORT;
                goto answer;
#endif
            }
            /* if we were asked to provide a context id, do so */
            if (coll->sig->assignID) {
                coll->sig->ctxid = prte_grpcomm_base.context_id;
                --prte_grpcomm_base.context_id;
                coll->sig->ctxid_assigned = true;
            }

            // construct the final membership
            PMIX_CONSTRUCT(&nmlist, pmix_list_t);
            // sadly, an exhaustive search
            for (m=0; m < coll->sig->nmembers; m++) {
                if (member_is_departed(coll, &coll->sig->members[m])) {
                    continue;
                }
                found = false;
                PMIX_LIST_FOREACH(nm, &nmlist, prte_namelist_t) {
                    if (PMIX_CHECK_PROCID(&coll->sig->members[m], &nm->name)) {
                        // if the new one is rank=WILDCARD, then ensure
                        // we keep it as wildcard
                        if (PMIX_RANK_WILDCARD == coll->sig->members[m].rank) {
                            nm->name.rank = PMIX_RANK_WILDCARD;
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    nm = PMIX_NEW(prte_namelist_t);
                    memcpy(&nm->name, &coll->sig->members[m], sizeof(pmix_proc_t));
                    pmix_list_append(&nmlist, &nm->super);
                }
            }
            // now check any added members
            for (m=0; m < coll->sig->naddmembers; m++) {
                if (member_is_departed(coll, &coll->sig->addmembers[m])) {
                    continue;
                }
                found = false;
                PMIX_LIST_FOREACH(nm, &nmlist, prte_namelist_t) {
                    if (PMIX_CHECK_PROCID(&coll->sig->addmembers[m], &nm->name)) {
                        // if the new one is rank=WILDCARD, then ensure
                        // we keep it as wildcard
                        if (PMIX_RANK_WILDCARD == coll->sig->addmembers[m].rank) {
                            nm->name.rank = PMIX_RANK_WILDCARD;
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    nm = PMIX_NEW(prte_namelist_t);
                    memcpy(&nm->name, &coll->sig->addmembers[m], sizeof(pmix_proc_t));
                    pmix_list_append(&nmlist, &nm->super);
                }
            }
            // create the full membership array
            nfinal = pmix_list_get_size(&nmlist);
            PMIX_PROC_CREATE(finalmembership, nfinal);
            m = 0;
            PMIX_LIST_FOREACH(nm, &nmlist, prte_namelist_t) {
                memcpy(&finalmembership[m], &nm->name, sizeof(pmix_proc_t));
                ++m;
            }
            PMIX_LIST_DESTRUCT(&nmlist);

            // if they gave us a final order, then sort the final membership
            // accordingly. Note that order entries that consist of nspace,wildcard
            // indicate that all participants from the given nspace should be
            // included in the final membership at that point - it does NOT mean
            // that all procs from that nspace are included in the final membership
            if (NULL != coll->sig->final_order) {
                PMIX_CONSTRUCT(&nmlist, pmix_list_t);
                for (m=0; m < coll->sig->nfinal; m++) {
                    // search the array of final members to capture those that match
                    for (n=0; n < nfinal; n++) {
                        if (PMIX_CHECK_PROCID(&coll->sig->final_order[m], &finalmembership[n])) {
                            // add this proc to the final list
                            nm = PMIX_NEW(prte_namelist_t);
                            memcpy(&nm->name, &finalmembership[n], sizeof(pmix_proc_t));
                            pmix_list_append(&nmlist, &nm->super);
                            // the final order may have included rank=wildcard - if so,
                            // then we have to continue
                            if (PMIX_RANK_WILDCARD != coll->sig->final_order[m].rank) {
                                // nope - can only match once
                                break;
                            }
                        }
                    }
                }
                // did we lose anyone?
                if (nfinal != pmix_list_get_size(&nmlist)) {
                    pmix_show_help("help-prte-runtime.txt", "bad-final-order", true);
                    coll->status = PMIX_ERR_BAD_PARAM;
                    goto answer;
                }
                // just overwrite the final array
                m = 0;
                PMIX_LIST_FOREACH(nm, &nmlist, prte_namelist_t) {
                    memcpy(&finalmembership[m], &nm->name, sizeof(pmix_proc_t));
                    ++m;
                }
                PMIX_LIST_DESTRUCT(&nmlist);

                // zero out the final order cache - no need to send it around
                PMIX_PROC_FREE(coll->sig->final_order, coll->sig->nfinal);
                coll->sig->final_order = NULL;
                coll->sig->nfinal = 0;

            } else {
                 /* sort the procs so everyone gets the same order */
                qsort(finalmembership, nfinal, sizeof(pmix_proc_t), pmix_util_compare_proc);
            }
            /* a group of nobody is not a group - if the failure took every
             * member, there is nothing left to construct */
            if (0 == nfinal) {
                coll->status = PMIX_GROUP_CONSTRUCT_ABORT;
                goto answer;
            }
        }

answer:
        // CONSTRUCT THE RELEASE MESSAGE
        PMIX_DATA_BUFFER_CREATE(reply);

        /* pack the signature */
        rc = pack_signature(reply, coll->sig);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            PMIX_PROC_FREE(finalmembership, nfinal);
            return;
        }
        /* pack the status */
        rc = PMIx_Data_pack(NULL, reply, &coll->status, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            PMIX_PROC_FREE(finalmembership, nfinal);
            return;
        }

        if (PMIX_GROUP_CONSTRUCT == coll->sig->op) {
            // pack the final membership
            rc = PMIx_Data_pack(NULL, reply, &nfinal, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                PMIX_PROC_FREE(finalmembership, nfinal);
                return;
            }
            if (0 < nfinal) {
                rc = PMIx_Data_pack(NULL, reply, finalmembership, nfinal, PMIX_PROC);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    PMIX_PROC_FREE(finalmembership, nfinal);
                    return;
                }
                PMIX_PROC_FREE(finalmembership, nfinal);
            }

            /* pack the members lost to a failed daemon, so each daemon can
             * tell its own clients who is missing from the membership */
            rc = PMIx_Data_pack(NULL, reply, &coll->ndeparted, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                return;
            }
            if (0 < coll->ndeparted) {
                rc = PMIx_Data_pack(NULL, reply, coll->departed,
                                    coll->ndeparted, PMIX_PROC);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    return;
                }
            }

            // pack any group info
            PMIx_Info_list_convert(coll->grpinfo, &darray);
            info = (pmix_info_t*)darray.array;
            ninfo = darray.size;
            rc = PMIx_Data_pack(NULL, reply, &ninfo, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                return;
            }
            if (0 < ninfo) {
               rc =  PMIx_Data_pack(NULL, reply, info, ninfo, PMIX_INFO);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    return;
                }
            }
            PMIX_DATA_ARRAY_DESTRUCT(&darray);

            // pack any endpts
            PMIx_Info_list_convert(coll->endpts, &darray);
            info = (pmix_info_t*)darray.array;
            ninfo = darray.size;
            rc = PMIx_Data_pack(NULL, reply, &ninfo, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                return;
            }
            if (0 < ninfo) {
                rc = PMIx_Data_pack(NULL, reply, info, ninfo, PMIX_INFO);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    return;
                }
            }
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
        }

        /* send the release via xcast - it copies the payload, so the
         * buffer is still ours to free */
        (void) prte_grpcomm.xcast(PRTE_RML_TAG_GROUP_RELEASE, reply);
        PMIX_DATA_BUFFER_RELEASE(reply);

    } else {
        PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:direct allgather rollup complete - sending to %s",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                             PRTE_NAME_PRINT(PRTE_PROC_MY_PARENT)));

        // setup to relay our rollup results
        PMIX_DATA_BUFFER_CREATE(reply);

        /* pack the signature */
        rc = pack_signature(reply, coll->sig);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }

        // pack the local collective status
        rc = PMIx_Data_pack(NULL, reply, &coll->status, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }

        // pack any timeout directive
        rc = PMIx_Data_pack(NULL, reply, &coll->timeout, 1, PMIX_INT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return;
        }

        if (PMIX_GROUP_CONSTRUCT == coll->sig->op) {
            // pack any group info
            PMIx_Info_list_convert(coll->grpinfo, &darray);
            info = (pmix_info_t*)darray.array;
            ninfo = darray.size;
            rc = PMIx_Data_pack(NULL, reply, &ninfo, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                return;
            }
            if (0 < ninfo) {
                rc = PMIx_Data_pack(NULL, reply, info, ninfo, PMIX_INFO);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    return;
                }
            }
            PMIX_DATA_ARRAY_DESTRUCT(&darray);

            // pack any endpts
            PMIx_Info_list_convert(coll->endpts, &darray);
            info = (pmix_info_t*)darray.array;
            ninfo = darray.size;
            rc = PMIx_Data_pack(NULL, reply, &ninfo, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_DATA_BUFFER_RELEASE(reply);
                return;
            }
            if (0 < ninfo) {
                rc =PMIx_Data_pack(NULL, reply, info, ninfo, PMIX_INFO);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DATA_BUFFER_RELEASE(reply);
                    return;
                }
            }
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
        }

        /* stamp our aggregate with the epoch it belongs to, so a parent
         * that has already recovered past it can tell it is stale */
        PMIX_DATA_BUFFER_CREATE(framed);
        rc = pack_epoch_frame(framed, reply);
        PMIX_DATA_BUFFER_RELEASE(reply);
        if (PRTE_SUCCESS != rc) {
            PMIX_DATA_BUFFER_RELEASE(framed);
            return;
        }

        /* send the info to our parent */
        PRTE_RML_SEND(rc, PRTE_PROC_MY_PARENT->rank, framed,
                      PRTE_RML_TAG_GROUP);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(framed);
            return;
        }
    }
}

static void relcb(void *cbdata)
{
    prte_pmix_grp_caddy_t *cd = (prte_pmix_grp_caddy_t*)cbdata;
    PMIX_RELEASE(cd);
}

/* Carrier for the second half of grp_release.
 *
 * A construct's resources are registered with our local PMIx server BEFORE
 * we tell our local clients the construct completed, so that anything asking
 * that server about the group finds it already there.  That ordering used to
 * be enforced by waiting on the registration, but we must not wait here:
 * grp_release runs on the progress thread, and prte_event_base
 * has no thread of its own (event.c: "PRTE tools block in their own loop over
 * the event base"), so parking it makes the daemon deaf - no RML, no IOF, no
 * other job's state transitions - for the duration of the registration.  That
 * is not a once-per-job teardown cost either: this path runs on every daemon
 * for every group construct, so a session-heavy job pays it repeatedly.
 *
 * The ordering is therefore expressed as a continuation: the caddy carries
 * everything the remainder of grp_release needs, and the registration's
 * completion callback hands it back to our progress thread.
 */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    prte_grpcomm_direct_group_signature_t *sig;
    pmix_status_t st;
    void *nlist;
    pmix_info_t *info;
    size_t ninfo;
    pmix_proc_t *finalmembership;
    size_t nfinal;
    pmix_proc_t *departed;
    size_t ndeparted;
    pmix_info_t *grpinfo;
    size_t ngrpinfo;
    pmix_info_t *endpts;
    size_t nendpts;
} prte_grpcomm_release_caddy_t;

static void rlcon(prte_grpcomm_release_caddy_t *p)
{
    p->sig = NULL;
    p->st = PMIX_SUCCESS;
    p->nlist = NULL;
    p->info = NULL;
    p->ninfo = 0;
    p->finalmembership = NULL;
    p->departed = NULL;
    p->ndeparted = 0;
    p->nfinal = 0;
    p->grpinfo = NULL;
    p->ngrpinfo = 0;
    p->endpts = NULL;
    p->nendpts = 0;
}
static void rldes(prte_grpcomm_release_caddy_t *p)
{
    if (NULL != p->info) {
        PMIX_INFO_FREE(p->info, p->ninfo);
    }
    if (NULL != p->grpinfo) {
        PMIX_INFO_FREE(p->grpinfo, p->ngrpinfo);
    }
    if (NULL != p->endpts) {
        PMIX_INFO_FREE(p->endpts, p->nendpts);
    }
    if (NULL != p->finalmembership) {
        PMIX_PROC_FREE(p->finalmembership, p->nfinal);
    }
    if (NULL != p->departed) {
        PMIX_PROC_FREE(p->departed, p->ndeparted);
    }
    if (NULL != p->nlist) {
        PMIX_INFO_LIST_RELEASE(p->nlist);
    }
    if (NULL != p->sig) {
        PMIX_RELEASE(p->sig);
    }
}
static PMIX_CLASS_INSTANCE(prte_grpcomm_release_caddy_t, pmix_object_t,
                           rlcon, rldes);

static void grp_release_complete(prte_grpcomm_release_caddy_t *cd);

/* Resumption point for the continuation - runs on the PRRTE progress
 * thread, where every object the completion touches belongs. */
static void grp_release_resume(int fd, short args, void *cbdata)
{
    prte_grpcomm_release_caddy_t *cd = (prte_grpcomm_release_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(cd);
    grp_release_complete(cd);
}

/* Completion of the resource registration.  Runs on the PMIx progress
 * thread, so it does nothing but record the status and hand the caddy
 * back to ours. */
static void grp_release_regcbfunc(pmix_status_t status, void *cbdata)
{
    prte_grpcomm_release_caddy_t *cd = (prte_grpcomm_release_caddy_t *) cbdata;

    /* a group whose resources did not make it into our server is not a
     * group our local clients can use - report the failure rather than
     * handing them one whose endpoint lookups will quietly come up empty */
    if (PMIX_SUCCESS != status) {
        cd->st = status;
    }
    PRTE_PMIX_THREADSHIFT(cd, prte_event_base, grp_release_resume);
}

/* Has this operation already been released here? */
static bool group_op_completed(prte_grpcomm_direct_group_signature_t *sig)
{
    prte_grpcomm_group_memo_t *memo;

    PMIX_LIST_FOREACH(memo, &prte_mca_grpcomm_direct_component.completed_group_ops,
                      prte_grpcomm_group_memo_t) {
        if (sig->op == memo->op && 0 == strcmp(sig->groupID, memo->groupID)) {
            return true;
        }
    }
    return false;
}

/* Forget any record of this operation, so a fresh one of the same name and
 * type can run. Called when a local client starts one: the client asking is
 * proof that the previous operation of that name is over and done with. */
static void group_op_forget(prte_grpcomm_direct_group_signature_t *sig)
{
    prte_grpcomm_group_memo_t *memo, *nxt;

    PMIX_LIST_FOREACH_SAFE(memo, nxt, &prte_mca_grpcomm_direct_component.completed_group_ops,
                           prte_grpcomm_group_memo_t) {
        if (sig->op == memo->op && 0 == strcmp(sig->groupID, memo->groupID)) {
            pmix_list_remove_item(&prte_mca_grpcomm_direct_component.completed_group_ops,
                                  &memo->super);
            PMIX_RELEASE(memo);
        }
    }
}

/* Record that we released this operation, evicting the oldest entry once the
 * memo is full - it only has to outlive messages still in flight. */
static void group_op_remember(prte_grpcomm_direct_group_signature_t *sig)
{
    prte_grpcomm_group_memo_t *memo;

    if (group_op_completed(sig)) {
        return;
    }
    while (PRTE_GRPCOMM_GROUP_MEMO_MAX <=
           pmix_list_get_size(&prte_mca_grpcomm_direct_component.completed_group_ops)) {
        memo = (prte_grpcomm_group_memo_t *)
            pmix_list_remove_first(&prte_mca_grpcomm_direct_component.completed_group_ops);
        PMIX_RELEASE(memo);
    }
    memo = PMIX_NEW(prte_grpcomm_group_memo_t);
    memo->groupID = strdup(sig->groupID);
    memo->op = sig->op;
    pmix_list_append(&prte_mca_grpcomm_direct_component.completed_group_ops, &memo->super);
}

static void find_delete_tracker(prte_grpcomm_direct_group_signature_t *sig)
{
    prte_grpcomm_group_t *coll;

    group_op_remember(sig);

    PMIX_LIST_FOREACH(coll, &prte_mca_grpcomm_direct_component.group_ops, prte_grpcomm_group_t) {
        // must match both groupID and operation - the same key get_tracker
        // uses. A groupID alone does not identify a tracker: a construct and
        // a destruct of the same group are distinct operations, and matching
        // on the name alone lets one operation's release delete the other's
        // tracker.
        if (0 == strcmp(sig->groupID, coll->sig->groupID) &&
            sig->op == coll->sig->op) {
            pmix_list_remove_item(&prte_mca_grpcomm_direct_component.group_ops, &coll->super);
            PMIX_RELEASE(coll);
            return;
        }
    }
}

void prte_grpcomm_direct_grp_release(int status, pmix_proc_t *sender,
                                     pmix_data_buffer_t *buffer,
                                     prte_rml_tag_t tag, void *cbdata)
{
    prte_grpcomm_group_t *coll;
    prte_grpcomm_direct_group_signature_t *sig = NULL;
    prte_grpcomm_release_caddy_t *cd;
    int32_t cnt;
    pmix_status_t rc = PMIX_SUCCESS, st = PMIX_SUCCESS;
    size_t n;
    pmix_data_array_t darray;
    prte_pmix_server_pset_t *pset;
    void *ilist;
    PRTE_HIDE_UNUSED_PARAMS(status, sender, tag, cbdata);

    pmix_output_verbose(2, prte_pmix_server_globals.output,
                        "%s group release recvd",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    // unpack the signature
    rc = unpack_signature(buffer, &sig);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
        return;
    }

    // unpack the status
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &st, &cnt, PMIX_STATUS);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        st = rc;
    }

    /* if this was a destruct operation, then there is nothing
     * further to unpack */
    if (PMIX_GROUP_DESTRUCT == sig->op) {
        /* check for the tracker - okay if not found, it just
         * means that we had no local participants */
        coll = get_tracker(sig, false);
        /* find this group ID on our list of groups */
        PMIX_LIST_FOREACH(pset, &prte_pmix_server_globals.groups, prte_pmix_server_pset_t)
        {
            if (0 == strcmp(pset->name, sig->groupID)) {
                pmix_list_remove_item(&prte_pmix_server_globals.groups, &pset->super);
                PMIX_RELEASE(pset);
                break;
            }
        }
        if (NULL != coll && NULL != coll->cbfunc) {
            /* return to the local procs in the collective */
            coll->cbfunc(st, NULL, 0, coll->cbdata, NULL, NULL);
        }
        // remove the tracker, if found
        find_delete_tracker(sig);
        PMIX_RELEASE(sig);
        return;
    }

    // setup to cache info
    ilist = PMIx_Info_list_start();

    /* everything from here on has to survive the registration of the
     * group's resources with our local PMIx server, which we are not
     * allowed to wait for - hand it to the continuation caddy, which
     * takes over ownership of the signature as well */
    cd = PMIX_NEW(prte_grpcomm_release_caddy_t);
    cd->sig = sig;
    cd->st = st;
    cd->nlist = PMIx_Info_list_start();

    // must be a construct operation - continue unpacking
    if (PMIX_SUCCESS != cd->st) {
        PMIX_INFO_LIST_RELEASE(ilist);
        goto notify;
    }

    if (sig->ctxid_assigned) {
        PMIX_INFO_LIST_ADD(rc, ilist, PMIX_GROUP_CONTEXT_ID, &sig->ctxid, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            cd->st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            goto notify;
        }
        PMIX_INFO_LIST_ADD(rc, cd->nlist, PMIX_GROUP_CONTEXT_ID, &sig->ctxid, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            cd->st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            goto notify;
        }
    }

    // unpack the final membership
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &cd->nfinal, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        cd->st = rc;
        cd->nfinal = 0;
        PMIX_INFO_LIST_RELEASE(ilist);
        goto notify;
    }
    if (0 < cd->nfinal) {
        PMIX_PROC_CREATE(cd->finalmembership, cd->nfinal);
        cnt = cd->nfinal;
        rc = PMIx_Data_unpack(NULL, buffer, cd->finalmembership, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            cd->st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            goto notify;
        }
        // pass back the final group membership
        darray.type = PMIX_PROC;
        darray.array = cd->finalmembership;
        darray.size = cd->nfinal;
        // load the array - note: this copies the array!
        PMIX_INFO_LIST_ADD(rc, cd->nlist, PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            cd->st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            goto notify;
        }
    }

    /* unpack the members lost to a failed daemon, if any */
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &cd->ndeparted, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        cd->st = rc;
        cd->ndeparted = 0;
        PMIX_INFO_LIST_RELEASE(ilist);
        goto notify;
    }
    if (0 < cd->ndeparted) {
        PMIX_PROC_CREATE(cd->departed, cd->ndeparted);
        cnt = cd->ndeparted;
        rc = PMIx_Data_unpack(NULL, buffer, cd->departed, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            cd->st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            goto notify;
        }
    }

    // unpack group info
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &cd->ngrpinfo, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        cd->st = rc;
        cd->ngrpinfo = 0;
        PMIX_INFO_LIST_RELEASE(ilist);
        goto notify;
    }
    if (0 < cd->ngrpinfo) {
        PMIX_INFO_CREATE(cd->grpinfo, cd->ngrpinfo);
        cnt = cd->ngrpinfo;
        rc = PMIx_Data_unpack(NULL, buffer, cd->grpinfo, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            cd->st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            goto notify;
        }
        // transfer them to both lists
        for (n=0; n < cd->ngrpinfo; n++) {
            rc = PMIx_Info_list_add_value(ilist, PMIX_GROUP_INFO, &cd->grpinfo[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                cd->st = rc;
                PMIX_INFO_LIST_RELEASE(ilist);
                goto notify;
            }
            rc = PMIx_Info_list_add_value(cd->nlist, PMIX_GROUP_INFO, &cd->grpinfo[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                cd->st = rc;
                PMIX_INFO_LIST_RELEASE(ilist);
                goto notify;
            }
        }
        PMIX_INFO_FREE(cd->grpinfo, cd->ngrpinfo);
    }


    // unpack endpts
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &cd->nendpts, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        cd->st = rc;
        cd->nendpts = 0;
        PMIX_INFO_LIST_RELEASE(ilist);
        goto notify;
    }
    if (0 < cd->nendpts) {
        PMIX_INFO_CREATE(cd->endpts, cd->nendpts);
        cnt = cd->nendpts;
        rc = PMIx_Data_unpack(NULL, buffer, cd->endpts, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            cd->st = rc;
            PMIX_INFO_LIST_RELEASE(ilist);
            goto notify;
        }
        // transfer them to both lists
        for (n=0; n < cd->nendpts; n++) {
            rc = PMIx_Info_list_add_value(ilist, PMIX_GROUP_ENDPT_DATA, &cd->endpts[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                cd->st = rc;
                PMIX_INFO_LIST_RELEASE(ilist);
                goto notify;
            }
            rc = PMIx_Info_list_add_value(cd->nlist, PMIX_GROUP_ENDPT_DATA, &cd->endpts[n].value);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                cd->st = rc;
                PMIX_INFO_LIST_RELEASE(ilist);
                goto notify;
            }
        }
        PMIX_INFO_FREE(cd->endpts, cd->nendpts);
    }

    // PRRTE automatically ensures that all daemons register all jobs
    // with their local PMIx server, regardless of whether or not
    // that daemon hosts any local clients of that job. So we
    // do not need to collect/pass job data for participants
    // in the group construct

    /* pass the information down to the PMIx server.  This completes before
     * we service our local clients, so that anything asking that server
     * about the group finds it already there - but the rest of this
     * operation runs as a continuation off the registration rather than
     * waiting on it here. */
    PMIX_INFO_LIST_CONVERT(rc, ilist, &darray);
    PMIX_INFO_LIST_RELEASE(ilist);
    if (PMIX_SUCCESS != rc) {
        /* an empty list just means the construct carried nothing the server
         * needs to cache, so there is simply nothing to register - anything
         * else is a real failure.  Either way darray was left untouched, so
         * we must not reach for it. */
        if (PMIX_ERR_EMPTY != rc) {
            PMIX_ERROR_LOG(rc);
            cd->st = rc;
        }
        goto notify;
    }
    cd->info = (pmix_info_t *) darray.array;
    cd->ninfo = darray.size;

    rc = PMIx_server_register_resources(cd->info, cd->ninfo,
                                        grp_release_regcbfunc, cd);
    if (PMIX_SUCCESS == rc) {
        /* the completion callback owns the caddy now */
        return;
    }
    if (PMIX_OPERATION_SUCCEEDED != rc) {
        PMIX_ERROR_LOG(rc);
        cd->st = rc;
    }
    /* it completed immediately, and we are already on the progress
     * thread - so just carry straight on */

notify:
    grp_release_complete(cd);
}

#if PRTE_PMIX_HAVE_GROUP_FT
/* Raise PMIX_GROUP_MEMBER_FAILED, once per lost member, to this node's members
 * of the group.
 *
 * The range is custom rather than local: PMIX_RANGE_LOCAL would reach every
 * client on the node, including those belonging to jobs with no interest in
 * this group. Build the target list from the final membership, keeping the
 * procs that are actually running here.
 *
 * "prte.notify.donotloop" is required, not optional: without it PMIx hands the
 * event back to our own notify_event upcall, which thread-shifts onto the
 * progress thread - the thread we are on and are blocking inside
 * PMIx_Notify_event - and the daemon deadlocks. */
static void notify_members_lost(prte_grpcomm_release_caddy_t *cd)
{
    pmix_proc_t *targets;
    size_t ntargets = 0, n;
    prte_proc_t *p;
    pmix_data_array_t darray;
    pmix_info_t info[4];
    pmix_status_t rc;

    if (0 == cd->nfinal) {
        return;
    }
    PMIX_PROC_CREATE(targets, cd->nfinal);
    for (n = 0; n < cd->nfinal; n++) {
        if (PMIX_RANK_WILDCARD == cd->finalmembership[n].rank) {
            continue;
        }
        p = prte_get_proc_object(&cd->finalmembership[n]);
        if (NULL == p || !PRTE_FLAG_TEST(p, PRTE_PROC_FLAG_LOCAL)) {
            continue;
        }
        memcpy(&targets[ntargets], &cd->finalmembership[n], sizeof(pmix_proc_t));
        ++ntargets;
    }
    if (0 == ntargets) {
        PMIX_PROC_FREE(targets, cd->nfinal);
        return;
    }

    for (n = 0; n < cd->ndeparted; n++) {
        darray.type = PMIX_PROC;
        darray.array = targets;
        darray.size = ntargets;
        PMIX_INFO_LOAD(&info[0], PMIX_EVENT_AFFECTED_PROC, &cd->departed[n], PMIX_PROC);
        PMIX_INFO_LOAD(&info[1], PMIX_GROUP_ID, cd->sig->groupID, PMIX_STRING);
        PMIX_INFO_LOAD(&info[2], PMIX_EVENT_CUSTOM_RANGE, &darray, PMIX_DATA_ARRAY);
        PMIX_INFO_LOAD(&info[3], "prte.notify.donotloop", NULL, PMIX_BOOL);
        rc = PMIx_Notify_event(PMIX_GROUP_MEMBER_FAILED, &prte_process_info.myproc,
                               PMIX_RANGE_CUSTOM, info, 4, NULL, NULL);
        if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
            PMIX_ERROR_LOG(rc);
        }
        PMIX_INFO_DESTRUCT(&info[0]);
        PMIX_INFO_DESTRUCT(&info[1]);
        PMIX_INFO_DESTRUCT(&info[2]);
        PMIX_INFO_DESTRUCT(&info[3]);
    }
    PMIX_PROC_FREE(targets, cd->nfinal);
}
#endif /* PRTE_PMIX_HAVE_GROUP_FT */

/* The remainder of grp_release, run once the group's resources are in our
 * local PMIx server.  Always executes on the PRRTE progress thread. */
static void grp_release_complete(prte_grpcomm_release_caddy_t *cd)
{
    prte_grpcomm_group_t *coll;
    prte_pmix_grp_caddy_t *ccd;
    prte_pmix_server_pset_t *pset;
    pmix_data_array_t darray;
    pmix_status_t rc;

    if (PMIX_SUCCESS == cd->st) {
       /* add it to our list of known groups */
        pset = PMIX_NEW(prte_pmix_server_pset_t);
        pset->name = strdup(cd->sig->groupID);
        if (NULL != cd->finalmembership) {
            pset->num_members = cd->nfinal;
            PMIX_PROC_CREATE(pset->members, pset->num_members);
            memcpy(pset->members, cd->finalmembership,
                   cd->nfinal * sizeof(pmix_proc_t));
        }
        pmix_list_append(&prte_pmix_server_globals.groups, &pset->super);
    }

    /* Look the tracker up now instead of carrying it across the
     * registration: the progress thread was free while PMIx worked, so an
     * abort or a duplicate release could have completed and deleted it in
     * the interim - in which case our local participants have already been
     * serviced and there is nothing left here to do.  It is equally fine
     * for it to have never existed, which simply means we had no local
     * participants. */
    coll = get_tracker(cd->sig, false);

    // regardless of prior error, we MUST notify any pending clients
    // so they don't hang

    if (NULL != coll && NULL != coll->cbfunc) {
        // service the procs that are part of the collective

        // convert for returning to PMIx server library
        ccd = PMIX_NEW(prte_pmix_grp_caddy_t);
        if (PMIX_SUCCESS == cd->st) {
            PMIX_INFO_LIST_CONVERT(rc, cd->nlist, &darray);
            if (PMIX_SUCCESS != rc) {
                if (PMIX_ERR_EMPTY != rc) {
                    PMIX_ERROR_LOG(rc);
                }
            } else {
                ccd->info = (pmix_info_t*)darray.array;
                ccd->ninfo = darray.size;
            }
        }

        /* return to the PMIx server library for relay to
         * local procs in the operation */
        coll->cbfunc(cd->st, ccd->info, ccd->ninfo, coll->cbdata, relcb, (void*)ccd);

        /* Tell our own participants who they lost. This is done after the
         * construct's completion has been dispatched, so a client sees
         * PMIx_Group_construct return before it hears about the casualties,
         * and only on a daemon that actually has participants - grp_release
         * runs on every daemon in the DVM, and an untargeted notification
         * would reach clients of unrelated jobs.
         *
         * PMIx cannot do this for us: its own equivalent walks the block's
         * list of departed members, which is filled in from local client
         * connections dropping, and a surviving server never learns anything
         * about a process on a daemon that died. */
#if PRTE_PMIX_HAVE_GROUP_FT
        if (PMIX_SUCCESS == cd->st && 0 < cd->ndeparted) {
            notify_members_lost(cd);
        }
#endif
    }

    // remove this collective from our tracker
    find_delete_tracker(cd->sig);
    /* the caddy owns the signature and everything we unpacked into it */
    PMIX_RELEASE(cd);
}


static prte_grpcomm_group_t *get_tracker(prte_grpcomm_direct_group_signature_t *sig,
                                         bool create)
{
    prte_grpcomm_group_t *coll;
    int rc;
    pmix_proc_t *p;
    size_t n, nmb;
    pmix_list_t plist;
    prte_namelist_t *nm;
    bool found;

    if (NULL == sig->groupID) {
        return NULL;
    }

    /* search the existing tracker list to see if this already exists - we
     * default to using the groupID if one is given, otherwise we fallback
     * to the array of participating procs */
    PMIX_LIST_FOREACH(coll, &prte_mca_grpcomm_direct_component.group_ops, prte_grpcomm_group_t) {
        // must match groupID's and ops
        if (0 == strcmp(sig->groupID, coll->sig->groupID) &&
            sig->op == coll->sig->op) {
            pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:group:returning existing collective %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 sig->groupID);
            // if this is a bootstrap, then we have to track the number of leaders
            if (0 < sig->bootstrap) {
                if (0 < coll->nleaders) {
                    if (coll->nleaders != sig->bootstrap) {
                        // this is an error
                        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                        return NULL;
                    }
                } else {
                    // collective tracker could have been created by a follower,
                    // which means nleaders will not have been set
                    coll->nleaders = sig->bootstrap;
                }
                coll->bootstrap = true;
                // add this proc to the list of members
                PMIX_CONSTRUCT(&plist, pmix_list_t);
                for (n=0; n < sig->nmembers; n++) {
                    // see if we already have this proc
                    found = false;
                    for (nmb=0; nmb < coll->sig->nmembers; nmb++) {
                        if (PMIX_CHECK_PROCID(&sig->members[n], &coll->sig->members[nmb])) {
                            // yes, we do
                            found = true;
                            // check for wildcard as that needs to be retained.
                            // note that "n" indexes the incoming signature while
                            // "nmb" indexes the tracker's - the retention must be
                            // applied to the entry we matched, not to whatever
                            // happens to sit at the same offset over here
                            if (PMIX_RANK_WILDCARD == sig->members[n].rank) {
                                coll->sig->members[nmb].rank = PMIX_RANK_WILDCARD;
                            }
                            break;
                        }
                    }
                    if (!found) {
                        // cache the proc
                        nm = PMIX_NEW(prte_namelist_t);
                        memcpy(&nm->name, &sig->members[n], sizeof(pmix_proc_t));
                        pmix_list_append(&plist, &nm->super);
                    }
                }
                // add any missing procs to the addmembers
                if (0 < pmix_list_get_size(&plist)) {
                    n = coll->sig->nmembers + pmix_list_get_size(&plist);
                    PMIX_PROC_CREATE(p, n);
                    if (NULL != coll->sig->members) {
                        memcpy(p, coll->sig->members, coll->sig->nmembers * sizeof(pmix_proc_t));
                    }
                    n = coll->sig->nmembers;
                    PMIX_LIST_FOREACH(nm, &plist, prte_namelist_t) {
                        memcpy(&p[n], &nm->name, sizeof(pmix_proc_t));
                        ++n;
                    }
                    PMIX_LIST_DESTRUCT(&plist);
                    if (NULL != coll->sig->members) {
                        PMIX_PROC_FREE(coll->sig->members, coll->sig->nmembers);
                    }
                    coll->sig->members = p;
                    coll->sig->nmembers = n;
                }

            } else if (sig->follower) {
                // just ensure the bootstrap flag is set
                coll->bootstrap = true;
            }

            // if we are adding members, aggregate them
            if (0 < sig->naddmembers) {
                PMIX_CONSTRUCT(&plist, pmix_list_t);
                for (n=0; n < sig->naddmembers; n++) {
                    // see if we already have this proc
                    found = false;
                    for (nmb=0; nmb < coll->sig->naddmembers; nmb++) {
                        if (PMIX_CHECK_PROCID(&sig->addmembers[n], &coll->sig->addmembers[nmb])) {
                            // yes, we do
                            found = true;
                            // check for wildcard as that needs to be retained -
                            // see the note above on the members loop: "nmb" is
                            // the index into the tracker's array
                            if (PMIX_RANK_WILDCARD == sig->addmembers[n].rank) {
                                coll->sig->addmembers[nmb].rank = PMIX_RANK_WILDCARD;
                            }
                            break;
                        }
                    }
                    if (!found) {
                        // cache the proc
                        nm = PMIX_NEW(prte_namelist_t);
                        memcpy(&nm->name, &sig->addmembers[n], sizeof(pmix_proc_t));
                        pmix_list_append(&plist, &nm->super);
                    }
                }
                // add any missing procs to the addmembers
                if (0 < pmix_list_get_size(&plist)) {
                    n = coll->sig->naddmembers + pmix_list_get_size(&plist);
                    PMIX_PROC_CREATE(p, n);
                    if (NULL != coll->sig->addmembers) {
                        memcpy(p, coll->sig->addmembers, coll->sig->naddmembers * sizeof(pmix_proc_t));
                    }
                    n = coll->sig->naddmembers;
                    PMIX_LIST_FOREACH(nm, &plist, prte_namelist_t) {
                        memcpy(&p[n], &nm->name, sizeof(pmix_proc_t));
                        ++n;
                    }
                    PMIX_LIST_DESTRUCT(&plist);
                    if (NULL != coll->sig->addmembers) {
                        PMIX_PROC_FREE(coll->sig->addmembers, coll->sig->naddmembers);
                    }
                    coll->sig->addmembers = p;
                    coll->sig->naddmembers = n;
                    coll->nfollowers = n;
                }
            }
            // if they specified a final order, see if one was already given
            if (NULL != sig->final_order) {
                if (NULL == coll->sig->final_order) {
                    // cache the directive
                    PMIX_PROC_CREATE(coll->sig->final_order, sig->nfinal);
                    memcpy(coll->sig->final_order, sig->final_order, sig->nfinal * sizeof(pmix_proc_t));
                    coll->sig->nfinal = sig->nfinal;
                } else {
                    // see if they match - for now, do a direct match
                    if (coll->sig->nfinal != sig->nfinal) {
                        // this is an error
                        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                        return NULL;
                    }
                    if (0 != memcmp(coll->sig->final_order, sig->final_order, sig->nfinal * sizeof(pmix_proc_t))) {
                        // this is an error
                        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
                        return NULL;
                    }
                    // they are the same, so just ignore the new directive
                }
            }
            if (!coll->sig->assignID && sig->assignID) {
                coll->sig->assignID = true;
            }
            /* Sticky-OR: one participant asking for a fault-tolerant
             * collective makes the whole operation one. Note this is a
             * deliberate superset of the PMIx server's own rule, which takes
             * the first PMIX_GROUP_FT_COLLECTIVE it finds in the aggregated
             * block info and ignores the rest - accumulating is the more
             * predictable of the two when participants disagree. */
            if (!coll->sig->ft_collective && sig->ft_collective) {
                coll->sig->ft_collective = true;
            }
            return coll;
        }
    }

    /* if we get here, then this is a new collective - so create
     * the tracker for it unless directed otherwise */
    if (!create) {
        pmix_output_verbose(1, prte_grpcomm_base_framework.framework_output,
                             "%s grpcomm:base: not creating new coll",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

        return NULL;
    }

    coll = PMIX_NEW(prte_grpcomm_group_t);
    coll->sig = PMIX_NEW(prte_grpcomm_direct_group_signature_t);
    coll->sig->op = sig->op;
    coll->sig->groupID = strdup(sig->groupID);
    coll->sig->assignID = sig->assignID;
    // save the participating procs
    coll->sig->nmembers = sig->nmembers;
    if (0 < sig->nmembers) {
        coll->sig->members = (pmix_proc_t *) malloc(coll->sig->nmembers * sizeof(pmix_proc_t));
        memcpy(coll->sig->members, sig->members, coll->sig->nmembers * sizeof(pmix_proc_t));
    }
    coll->sig->naddmembers = sig->naddmembers;
    if (0 < sig->naddmembers) {
        coll->sig->addmembers = (pmix_proc_t *) malloc(coll->sig->naddmembers * sizeof(pmix_proc_t));
        memcpy(coll->sig->addmembers, sig->addmembers, coll->sig->naddmembers * sizeof(pmix_proc_t));
    }
    // save any final membership order that was given - the match path
    // above caches it, so failing to do so here would silently discard an
    // order supplied by whichever daemon happens to create the tracker
    if (NULL != sig->final_order) {
        PMIX_PROC_CREATE(coll->sig->final_order, sig->nfinal);
        memcpy(coll->sig->final_order, sig->final_order, sig->nfinal * sizeof(pmix_proc_t));
        coll->sig->nfinal = sig->nfinal;
    }
    coll->nfollowers = coll->sig->naddmembers;
    // the accumulated signature is what we later pack onto the wire, so it
    // has to carry the bootstrap description too
    coll->sig->bootstrap = sig->bootstrap;
    coll->sig->follower = sig->follower;
    coll->sig->ft_collective = sig->ft_collective;
    // need to know the bootstrap in case one is ongoing
    coll->nleaders = coll->sig->bootstrap;
    if (0 < sig->bootstrap || sig->follower) {
        coll->bootstrap = true;
    }
    pmix_list_append(&prte_mca_grpcomm_direct_component.group_ops, &coll->super);

    /* if this is a bootstrap operation, then there is no "rollup"
     * collective - each daemon reports directly to the DVM controller */
    if (coll->bootstrap) {
        return coll;
    }

    /* now get the daemons involved */
    if (PRTE_SUCCESS != (rc = create_dmns(sig, &coll->dmns, &coll->ndmns))) {
        PRTE_ERROR_LOG(rc);
        pmix_list_remove_item(&prte_mca_grpcomm_direct_component.group_ops, &coll->super);
        PMIX_RELEASE(coll);
        return NULL;
    }

    /* count the number of contributions we should get */
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

    return coll;
}

/* Collect the members lost with a failed daemon. Wildcard members are expanded
 * through the job map here, because the final membership keeps the wildcard
 * entry as-is - we cannot prune a namespace proc-by-proc - but the clients
 * still deserve to be told which specific processes went away. */
#if PRTE_PMIX_HAVE_GROUP_FT
static void collect_departed(prte_grpcomm_group_t *coll)
{
    pmix_list_t dl;
    prte_namelist_t *nm;
    prte_job_t *jdata;
    prte_proc_t *proc;
    pmix_proc_t *set[2];
    size_t nset[2], k, n, m;
    int i;

    PMIX_CONSTRUCT(&dl, pmix_list_t);
    set[0] = coll->sig->members;
    nset[0] = coll->sig->nmembers;
    set[1] = coll->sig->addmembers;
    nset[1] = coll->sig->naddmembers;

    for (k = 0; k < 2; k++) {
        for (n = 0; n < nset[k]; n++) {
            if (PMIX_RANK_WILDCARD != set[k][n].rank) {
                if (prte_grpcomm_direct_proc_departed(&set[k][n])) {
                    nm = PMIX_NEW(prte_namelist_t);
                    memcpy(&nm->name, &set[k][n], sizeof(pmix_proc_t));
                    pmix_list_append(&dl, &nm->super);
                }
                continue;
            }
            /* a wildcard - walk the namespace for procs on failed daemons */
            jdata = prte_get_job_data_object(set[k][n].nspace);
            if (NULL == jdata) {
                continue;
            }
            for (i = 0; i < jdata->procs->size; i++) {
                proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs, i);
                if (NULL == proc || NULL == proc->node || NULL == proc->node->daemon) {
                    continue;
                }
                if (!pmix_bitmap_is_set_bit(&prte_rml_base.failed_dmns,
                                            proc->node->daemon->name.rank)) {
                    continue;
                }
                nm = PMIX_NEW(prte_namelist_t);
                PMIX_LOAD_PROCID(&nm->name, set[k][n].nspace, proc->name.rank);
                pmix_list_append(&dl, &nm->super);
            }
        }
    }

    coll->ndeparted = pmix_list_get_size(&dl);
    if (0 < coll->ndeparted) {
        PMIX_PROC_CREATE(coll->departed, coll->ndeparted);
        m = 0;
        PMIX_LIST_FOREACH(nm, &dl, prte_namelist_t) {
            memcpy(&coll->departed[m], &nm->name, sizeof(pmix_proc_t));
            ++m;
        }
    }
    PMIX_LIST_DESTRUCT(&dl);
}
#endif /* PRTE_PMIX_HAVE_GROUP_FT */

static int create_dmns(prte_grpcomm_direct_group_signature_t *sig,
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
    /* Once the DVM has known failures, a member we cannot resolve is most
     * likely one whose node is being torn down, and refusing to resolve the
     * whole set would strand the collective: the caller drops the
     * contribution with no completion path. Skip what we cannot resolve and
     * carry on with the survivors. With no failures on record an
     * unresolvable member is still a genuine error, and still fatal. */
    bool tolerate = !pmix_bitmap_is_clear(&prte_rml_base.failed_dmns);

    PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                         "%s grpcomm:direct:group:create_dmns called with %s signature size %" PRIsize_t "",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                         (NULL == sig->members) ? "NULL" : "NON-NULL", sig->nmembers));

    PMIX_CONSTRUCT(&ds, pmix_list_t);
    for (n = 0; n < sig->nmembers; n++) {
        if (NULL == (jdata = prte_get_job_data_object(sig->members[n].nspace))) {
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
        if (PMIX_RANK_WILDCARD == sig->members[n].rank) {
            PMIX_OUTPUT_VERBOSE((1, prte_grpcomm_base_framework.framework_output,
                                 "%s grpcomm:direct:group::create_dmns called for all procs in job %s",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_JOBID_PRINT(sig->members[0].nspace)));
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
                                         "%s grpcomm:direct:group::create_dmns adding daemon %s to list",
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
                                 PRTE_NAME_PRINT(&sig->members[n])));
            proc = (prte_proc_t *) pmix_pointer_array_get_item(jdata->procs,
                                                               sig->members[n].rank);
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
                                 "%s grpcomm:direct:group::create_dmns adding daemon %s to array",
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

static int pack_signature(pmix_data_buffer_t *bkt,
                          prte_grpcomm_direct_group_signature_t *sig)
{
    pmix_status_t rc;

    // pack the operation
    rc = PMIx_Data_pack(NULL, bkt, &sig->op, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    // add the groupID
    rc = PMIx_Data_pack(NULL, bkt, &sig->groupID, 1, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    // pack the flag to assign context ID
    rc = PMIx_Data_pack(NULL, bkt, &sig->assignID, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    // pack the context ID, if one was given
    rc = PMIx_Data_pack(NULL, bkt, &sig->ctxid_assigned, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (sig->ctxid_assigned) {
        rc = PMIx_Data_pack(NULL, bkt, &sig->ctxid, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    // pack members, if given
    rc = PMIx_Data_pack(NULL, bkt, &sig->nmembers, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 < sig->nmembers) {
        rc = PMIx_Data_pack(NULL, bkt, sig->members, sig->nmembers, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    // pack bootstrap number
    rc = PMIx_Data_pack(NULL, bkt, &sig->bootstrap, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    // pack follower flag
    rc = PMIx_Data_pack(NULL, bkt, &sig->follower, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    // pack added membership, if given
    rc = PMIx_Data_pack(NULL, bkt, &sig->naddmembers, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 < sig->naddmembers) {
        rc = PMIx_Data_pack(NULL, bkt, sig->addmembers, sig->naddmembers, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    /* pack the fault-tolerant-collective flag. Deliberately not guarded by
     * PRTE_PMIX_HAVE_GROUP_FT: the wire format has to be identical across the
     * DVM regardless of what the PMIx we built against advertises */
    rc = PMIx_Data_pack(NULL, bkt, &sig->ft_collective, 1, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }

    // pack final order, if given
    rc = PMIx_Data_pack(NULL, bkt, &sig->nfinal, 1, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return prte_pmix_convert_status(rc);
    }
    if (0 < sig->nfinal) {
        rc = PMIx_Data_pack(NULL, bkt, sig->final_order, sig->nfinal, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return prte_pmix_convert_status(rc);
        }
    }

    return PRTE_SUCCESS;
}

static int unpack_signature(pmix_data_buffer_t *buffer,
                            prte_grpcomm_direct_group_signature_t **sig)
{
    pmix_status_t rc;
    int32_t cnt;
    prte_grpcomm_direct_group_signature_t *s;

    s = PMIX_NEW(prte_grpcomm_direct_group_signature_t);

    // unpack the op
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->op, &cnt, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }

    // unpack the groupID
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->groupID, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }

    // unpack whether or not to assign a context ID
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->assignID, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }

    // unpack the context ID, if one was assigned
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->ctxid_assigned, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }
    if (s->ctxid_assigned) {
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &s->ctxid, &cnt, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(s);
            return prte_pmix_convert_status(rc);
        }
    }

    // unpack the membership
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->nmembers, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }
    if (0 < s->nmembers) {
        PMIX_PROC_CREATE(s->members, s->nmembers);
        cnt = s->nmembers;
        rc = PMIx_Data_unpack(NULL, buffer, s->members, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(s);
            return prte_pmix_convert_status(rc);
        }
    }

    // unpack the bootstrap count
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->bootstrap, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }

    // unpack the follower flag
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->follower, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }

    // unpack the added members
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->naddmembers, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }
    if (0 < s->naddmembers) {
        PMIX_PROC_CREATE(s->addmembers, s->naddmembers);
        cnt = s->naddmembers;
        rc = PMIx_Data_unpack(NULL, buffer, s->addmembers, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(s);
            return prte_pmix_convert_status(rc);
        }
    }

    // unpack the fault-tolerant-collective flag - see pack_signature()
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->ft_collective, &cnt, PMIX_BOOL);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }

    // unpack the final order
    cnt = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &s->nfinal, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_RELEASE(s);
        return prte_pmix_convert_status(rc);
    }
    if (0 < s->nfinal) {
        PMIX_PROC_CREATE(s->final_order, s->nfinal);
        cnt = s->nfinal;
        rc = PMIx_Data_unpack(NULL, buffer, s->final_order, &cnt, PMIX_PROC);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(s);
            return prte_pmix_convert_status(rc);
        }
    }

    *sig = s;
    return PRTE_SUCCESS;
}
