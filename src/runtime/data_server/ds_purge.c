/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2016 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2025      Triad National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif

#include "src/class/pmix_pointer_array.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "src/runtime/data_server/prte_data_server.h"
#include "src/runtime/data_server/ds.h"

/* see the header for the rule this encodes */
bool prte_data_server_expires_by(pmix_persistence_t persist,
                                 pmix_persistence_t horizon)
{
    if (PMIX_PERSIST_INVALID == horizon) {
        return true;
    }
    switch (persist) {
    case PMIX_PERSIST_FIRST_READ:
        /* No lifetime ending takes this one.  Its criterion is the first
         * access and nothing else: an item published for a reader that has
         * not started yet is exactly what FIRST_READ is for, and the
         * publisher's own departure is not what the publisher asked to have
         * it removed by - PROC and APP are how one says that.  Taking it
         * here silently discarded a handover between one generation of a job
         * and the next, which is the one conforming way to pass a name
         * across a DVM that outlives both (issue #2733). */
        return false;
    case PMIX_PERSIST_PROC:
        /* the shortest lifetime we are ever told about: over by the time
         * any of them has ended */
        return true;
    case PMIX_PERSIST_APP:
        return (PMIX_PERSIST_APP == horizon ||
                PMIX_PERSIST_NSPACE == horizon ||
                PMIX_PERSIST_SESSION == horizon);
    case PMIX_PERSIST_NSPACE:
        return (PMIX_PERSIST_NSPACE == horizon || PMIX_PERSIST_SESSION == horizon);
    case PMIX_PERSIST_SESSION:
        return (PMIX_PERSIST_SESSION == horizon);
    case PMIX_PERSIST_INDEF:
    default:
        /* retained until specifically deleted */
        return false;
    }
}

/* Does the retention timeout apply to this item?
 *
 * Two persistences name no lifetime, so no horizon reclaims them and
 * nothing else would: PMIX_PERSIST_INDEF is retained until specifically
 * deleted, and only its publisher may delete it; PMIX_PERSIST_FIRST_READ is
 * consumed by a read that may never come.  Everything else has a criterion
 * a running system reaches, and cutting one short would break the retention
 * its publisher was promised while it is still alive to rely on it. */
static bool timeout_applies(prte_data_object_t *data)
{
    return (PMIX_PERSIST_INDEF == data->persistence ||
            PMIX_PERSIST_FIRST_READ == data->persistence);
}

static void sweep(int sd, short args, void *cbdata);

/* Interval between sweeps: often enough that "no earlier than the timeout,
 * and normally within a sweep interval after it" is a bound worth stating,
 * rare enough that an idle store costs nothing to keep. */
static void arm(void)
{
    struct timeval tv;
    int interval = prte_data_store.timeout / 4;

    if (1 > interval) {
        interval = 1;
    } else if (60 < interval) {
        interval = 60;
    }
    tv.tv_sec = interval;
    tv.tv_usec = 0;
    prte_event_evtimer_set(prte_event_base, &prte_data_store.sweep_ev, sweep, NULL);
    prte_data_store.sweep_active = true;
    prte_event_evtimer_add(&prte_data_store.sweep_ev, &tv);
}

void prte_ds_arm_sweep(void)
{
    int k;
    prte_data_object_t *data;

    if (0 >= prte_data_store.timeout || prte_data_store.sweep_active) {
        return;
    }
    for (k = 0; k < prte_data_store.store.size; k++) {
        data = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, k);
        if (NULL != data && timeout_applies(data)) {
            arm();
            return;
        }
    }
}

/* Remove what has gone stale, and stop sweeping once nothing is left that
 * could.  Runs on the progress thread inside the event loop, like every
 * other operation on this store, so it needs no locking. */
static void sweep(int sd, short args, void *cbdata)
{
    prte_data_object_t *data;
    time_t now = time(NULL);
    bool more = false;
    int k;
    PRTE_HIDE_UNUSED_PARAMS(sd, args, cbdata);

    prte_data_store.sweep_active = false;
    if (0 >= prte_data_store.timeout) {
        /* the parameter is read once at init, so this cannot change under
         * us - but a disabled timeout must not leave a sweep running */
        return;
    }

    for (k = 0; k < prte_data_store.store.size; k++) {
        data = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, k);
        if (NULL == data || !timeout_applies(data)) {
            continue;
        }
        if ((now - data->last_access) < prte_data_store.timeout) {
            more = true;
            continue;
        }
        pmix_output_verbose(1, prte_data_store.output,
                            "%s data server: %s data from %s expired after %ld idle seconds",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PMIx_Persistence_string(data->persistence),
                            PMIX_NAME_PRINT(&data->owner),
                            (long) (now - data->last_access));
        prte_ds_drop(data);
    }

    if (more) {
        arm();
    }
}

/* Does this item belong to the lifetime that just ended?
 *
 * The target says whose data is in question - a process, or with
 * PMIX_RANK_WILDCARD any rank of a namespace, or with an empty namespace
 * anybody at all, which is how the session horizon reaches across the jobs
 * that ran in it.  Two horizons then need something a process name cannot
 * carry, and that is what the qualifier is for. */
static bool purge_takes(prte_data_object_t *data, const pmix_proc_t *target,
                        pmix_persistence_t horizon, uint32_t qualifier)
{
    if (!PMIX_CHECK_PROCID(target, &data->owner)) {
        return false;
    }
    if (PMIX_PERSIST_APP == horizon && qualifier != data->app_idx) {
        /* another application of the same job, which is still running: the
         * namespace is not over just because this application is */
        return false;
    }
    if (PMIX_PERSIST_SESSION == horizon && qualifier != data->session_id) {
        return false;
    }
    return prte_data_server_expires_by(data->persistence, horizon);
}

/* Finish the lookups a process left parked when it ended.
 *
 * A parked request that outlives its requestor is worse than a leak.  A
 * later publish that satisfies it takes a PMIX_PERSIST_FIRST_READ value on
 * behalf of a process that cannot receive it, so the reader actually
 * waiting for that value - typically the next generation of the same job -
 * finds nothing.  And the requestor's daemon holds the request's room for
 * as long as it waits.
 *
 * So each one is answered - nobody reads the answer, but it is what lets
 * the daemon free that room - and released.
 *
 * Only the two horizons whose target names the requestors themselves.  An
 * APPLICATION's target is its whole namespace, and a parked request does not
 * say which application asked, so dropping by that target would cancel the
 * lookups of applications still running - and every process of the one that
 * ended was purged at the PROC horizon as it went.  A SESSION's target is
 * anybody at all.  An explicit PMIx_Unpublish(NULL, ...) ends nothing: it
 * comes from a live process that is taking its data back, and cancelling
 * the lookups it is waiting on is no part of that. */
static void drop_parked(const pmix_proc_t *target, pmix_persistence_t horizon)
{
    prte_data_req_t *req, *rqnext;

    if (PMIX_PERSIST_PROC != horizon && PMIX_PERSIST_NSPACE != horizon) {
        return;
    }
    PMIX_LIST_FOREACH_SAFE(req, rqnext, &prte_data_store.pending, prte_data_req_t) {
        /* strictly: a namespace we do not know is nobody's */
        if (!PMIX_CHECK_NSPACE_STRICT(target->nspace, req->requestor.nspace)) {
            continue;
        }
        if (PMIX_RANK_WILDCARD != target->rank && target->rank != req->requestor.rank) {
            continue;
        }
        pmix_output_verbose(1, prte_data_store.output,
                            "%s data server: dropping pending request from %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PMIX_NAME_PRINT(&req->requestor));
        pmix_list_remove_item(&prte_data_store.pending, &req->super);
        prte_ds_reply_parked(req, PMIX_ERR_NOT_FOUND, NULL);
        PMIX_RELEASE(req);
    }
}

/* Remove everything the ended lifetime takes.  Shared by the message form
 * and the direct one - what differs between them is who gets told, not what
 * goes. */
static void purge_store(const pmix_proc_t *target, pmix_persistence_t horizon,
                        uint32_t qualifier)
{
    prte_data_object_t *data;
    int k;

    for (k = 0; k < prte_data_store.store.size; k++) {
        data = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, k);
        if (NULL == data) {
            continue;
        }
        if (!purge_takes(data, target, horizon, qualifier)) {
            continue;
        }
        prte_ds_drop(data);
    }
    /* The parked lookups go with the lifetime too.  This used to happen
     * only in the message form, and once the state machine's purges became
     * calls into purge_local, nothing dropped them at all. */
    drop_parked(target, horizon);
}

void prte_data_server_purge_local(const pmix_proc_t *target,
                                  pmix_persistence_t horizon,
                                  uint32_t qualifier)
{
    /* A store nothing was ever published into is an array of one empty
     * slot, so this costs nothing in the case that matters: the PROC
     * horizon fires once per terminating process, and almost no job
     * publishes anything at all. */
    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: purge at %s horizon, data from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIx_Persistence_string(horizon),
                        PMIX_NAME_PRINT(target));
    purge_store(target, horizon, qualifier);
}

void prte_ds_purge(pmix_proc_t *sender,
                   pmix_data_buffer_t *buffer,
                   pmix_data_buffer_t *answer)
{
    int32_t count;
    pmix_status_t rc, ret;
    pmix_proc_t requestor;
    pmix_info_t *info;
    size_t n, ninfo;
    uint8_t u8;
    /* absent a PMIX_PERSISTENCE directive this is an explicit
     * "remove everything I published", not the end of a lifetime */
    pmix_persistence_t horizon = PMIX_PERSIST_INVALID;
    /* the app index or session id the horizon needs, where it needs one */
    uint32_t qualifier = UINT32_MAX, appidx = UINT32_MAX, sessionid = UINT32_MAX;

    /* unpack the process whose data is to be purged - PMIX_RANK_WILDCARD
     * for any rank of its namespace */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &requestor, &count, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    /* unpack the directives, if any */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        count = (int32_t) ninfo;
        rc = PMIx_Data_unpack(NULL, buffer, info, &count, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(info, ninfo);
            goto done;
        }
        /* Every one of these is read through its type rather than out of
         * the union regardless.  A purge is an unpublish naming no keys, so
         * the array can be a client's own, typed however the client typed
         * it - and an int holding PMIX_PERSIST_PROC read as a persist is
         * PMIX_PERSIST_INDEF on a big-endian host. */
        for (n = 0; n < ninfo; n++) {
            if (PMIx_Check_key(info[n].key, PMIX_PERSISTENCE)) {
                /* a lifetime ended, and this is which one */
                rc = prte_ds_get_named_uint8(&info[n].value, PMIX_PERSIST, &u8);
                if (PMIX_SUCCESS != rc) {
                    break;
                }
                horizon = u8;
            } else if (PMIx_Check_key(info[n].key, PRTE_PURGE_APP_IDX)) {
                rc = PMIx_Value_get_number(&info[n].value, &appidx, PMIX_UINT32);
                if (PMIX_SUCCESS != rc) {
                    break;
                }
            } else if (PMIx_Check_key(info[n].key, PMIX_SESSION_ID)) {
                rc = PMIx_Value_get_number(&info[n].value, &sessionid, PMIX_UINT32);
                if (PMIX_SUCCESS != rc) {
                    break;
                }
            }
        }
        if (PMIX_SUCCESS != rc) {
            /* a lifetime we cannot read is not one we may guess at - the
             * guess decides whose data goes */
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(info, ninfo);
            rc = PMIX_ERR_BAD_PARAM;
            goto done;
        }
        /* A relay purging on behalf of a process in its own DVM.  Without
         * this the purge would take everything the relay itself owns -
         * which is everything it ever published.  This one asks about the
         * process alone: a purge names whose data goes, and no uid enters
         * into it. */
        prte_ds_check_requestor(&requestor, NULL, NULL, info, ninfo);
        PMIX_INFO_FREE(info, ninfo);
    }

    /* Which of the two qualifiers counts is the horizon's to say, and a
     * horizon that needs one it was not given selects nothing real:
     * UINT32_MAX is what an item records when it has no application or
     * session, so it would take exactly those. */
    switch (horizon) {
    case PMIX_PERSIST_INVALID:
    case PMIX_PERSIST_PROC:
    case PMIX_PERSIST_NSPACE:
        break;
    case PMIX_PERSIST_APP:
        qualifier = appidx;
        break;
    case PMIX_PERSIST_SESSION:
        qualifier = sessionid;
        break;
    default:
        /* INDEF and FIRST_READ name no lifetime that can end, and anything
         * else names nothing at all */
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto done;
    }
    if ((PMIX_PERSIST_APP == horizon || PMIX_PERSIST_SESSION == horizon) &&
        UINT32_MAX == qualifier) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    /* A purge arriving as a message must name a namespace.  The empty one
     * is a wildcard to PMIX_CHECK_PROCID, which the session horizon relies
     * on when the MASTER purges its own store by a direct call - but a
     * session id means something only inside the DVM that assigned it.  A
     * relayed session purge used to arrive here naming "anybody", so another
     * DVM ending its session 1 took this DVM's own session-1 data, and every
     * lookup parked in the store was released unanswered, hanging the
     * processes waiting on them.  Nothing else sends an empty target: a
     * client's own name always has a namespace, and so does every other
     * horizon's. */
    if (PMIX_NSPACE_INVALID(requestor.nspace)) {
        rc = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(rc);
        goto done;
    }

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: purge data from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIX_NAME_PRINT(&requestor));

    /* Take what the ended lifetime takes, and finish any lookup it left
     * parked.  This is what makes PMIX_PERSISTENCE mean anything: the value
     * was recorded at publish and then never consulted again, so data
     * published to last only as long as its application sat in the store
     * until the DVM itself went away, and PMIX_PERSIST_APP and
     * PMIX_PERSIST_PROC both behaved as PMIX_PERSIST_INDEF. */
    purge_store(&requestor, horizon, qualifier);
    rc = PMIX_SUCCESS;

done:
    // send back an answer. Keep the pack status separate from the status
    // being reported: packing into the same variable we are packing FROM
    // discards the outcome the requestor asked about.
    ret = PMIx_Data_pack(NULL, answer, &rc, 1, PMIX_STATUS);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return;
    }
    PRTE_RML_RELIABLE_SEND(ret, sender->rank, answer, PRTE_RML_TAG_DATA_CLIENT);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
        PMIX_DATA_BUFFER_RELEASE(answer);
    }
}
