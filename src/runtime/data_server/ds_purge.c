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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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

#include <string.h>

#ifdef HAVE_SYS_TIME_H
#    include <sys/time.h>
#endif

#include "src/class/pmix_pointer_array.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
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
                        "%s data server: purge at %s horizon, data from %s:%d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIx_Persistence_string(horizon),
                        target->nspace, target->rank);
    purge_store(target, horizon, qualifier);
}

void prte_ds_purge(pmix_proc_t *sender,
                   pmix_data_buffer_t *buffer,
                   pmix_data_buffer_t *answer)
{
    int32_t count;
    pmix_status_t rc, ret;
    pmix_proc_t requestor;
    prte_data_req_t *req, *rqnext;
    pmix_info_t *info;
    size_t n, ninfo;
    /* absent a PMIX_PERSISTENCE directive this is an explicit
     * "remove everything I published", not the end of a lifetime */
    pmix_persistence_t horizon = PMIX_PERSIST_INVALID;
    /* the app index or session id the horizon needs, where it needs one */
    uint32_t qualifier = UINT32_MAX;

    /* unpack the proc whose data is to be purged - session
     * data is purged by providing a requestor whose rank
     * is wildcard */
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
        for (n = 0; n < ninfo; n++) {
            if (PMIx_Check_key(info[n].key, PMIX_PERSISTENCE)) {
                /* a lifetime ended, and this is which one */
                horizon = info[n].value.data.persist;
            } else if (PMIx_Check_key(info[n].key, PRTE_PURGE_APP_IDX) ||
                       PMIx_Check_key(info[n].key, PMIX_SESSION_ID)) {
                /* which application, or which session - the horizon says
                 * which of the two this is */
                qualifier = info[n].value.data.uint32;
            }
        }
        /* A relay purging on behalf of a process in its own DVM.  Without
         * this the purge would take everything the relay itself owns -
         * which is everything it ever published.  This one asks about the
         * process alone: a purge names whose data goes, and no uid enters
         * into it. */
        prte_ds_check_requestor(&requestor, NULL, NULL, info, ninfo);
        PMIX_INFO_FREE(info, ninfo);
    }

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: purge data from %s:%d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        requestor.nspace, requestor.rank);

    /* Take what the ended lifetime takes.  This is what makes
     * PMIX_PERSISTENCE mean anything: the value was recorded at publish and
     * then never consulted again, so data published to last only as long as
     * its application sat in the store until the DVM itself went away, and
     * PMIX_PERSIST_APP and PMIX_PERSIST_PROC both behaved as
     * PMIX_PERSIST_INDEF. */
    purge_store(&requestor, horizon, qualifier);

    /* Drop any lookup this process left parked on the pending list. Those
     * requests outlived their requestor: a later publish would match one and
     * try to reply to a process that no longer exists, and until then the
     * request kept the (already purged) proc's keys alive.
     *
     * Only when a lifetime actually ended, though.  An explicit
     * PMIx_Unpublish(NULL, ...) arrives as this same command from a process
     * that is very much alive, and cancelling the lookups it is waiting on
     * is no part of taking its published data back. */
    if (PMIX_PERSIST_INVALID == horizon) {
        goto done;
    }
    PMIX_LIST_FOREACH_SAFE(req, rqnext, &prte_data_store.pending, prte_data_req_t) {
        if (!PMIX_CHECK_PROCID(&requestor, &req->requestor)) {
            continue;
        }
        pmix_output_verbose(1, prte_data_store.output,
                            "%s data server: dropping pending request from %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PMIX_NAME_PRINT(&req->requestor));
        pmix_list_remove_item(&prte_data_store.pending, &req->super);
        PMIX_RELEASE(req);
    }

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
