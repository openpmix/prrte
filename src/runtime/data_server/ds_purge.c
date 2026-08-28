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
        return (PMIX_PERSIST_APP == horizon || PMIX_PERSIST_SESSION == horizon);
    case PMIX_PERSIST_SESSION:
        return (PMIX_PERSIST_SESSION == horizon);
    case PMIX_PERSIST_INDEF:
    default:
        /* retained until specifically deleted */
        return false;
    }
}

void prte_ds_purge(pmix_proc_t *sender,
                   pmix_data_buffer_t *buffer,
                   pmix_data_buffer_t *answer)
{
    int32_t count;
    prte_data_object_t *data;
    int k;
    pmix_status_t rc, ret;
    pmix_proc_t requestor;
    prte_data_req_t *req, *rqnext;
    pmix_info_t *info;
    size_t n, ninfo;
    /* absent a PMIX_PERSISTENCE directive this is an explicit
     * "remove everything I published", not the end of a lifetime */
    pmix_persistence_t horizon = PMIX_PERSIST_INVALID;

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
            if (PMIx_Check_key(info[n].key, PMIX_REQUESTOR)) {
                /* a relay purging on behalf of a process in its own DVM.
                 * Without this the purge would take everything the relay
                 * itself owns - which is everything it ever published. */
                prte_ds_check_requestor(&requestor, &info[n]);
            } else if (PMIx_Check_key(info[n].key, PMIX_PERSISTENCE)) {
                /* a lifetime ended, and this is which one */
                horizon = info[n].value.data.persist;
            }
        }
        PMIX_INFO_FREE(info, ninfo);
    }

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: purge data from %s:%d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        requestor.nspace, requestor.rank);

    /* cycle across the stored data, looking for a match */
    for (k = 0; k < prte_data_store.store.size; k++) {
        data = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, k);
        if (NULL == data) {
            continue;
        }
        /* check if data posted by the specified process */
        if (!PMIX_CHECK_PROCID(&requestor, &data->owner)) {
            continue;
        }
        /* ...and whether it was to outlive what just ended.  This is what
         * makes PMIX_PERSISTENCE mean anything: the value was recorded at
         * publish and then never consulted again, so data published to last
         * only as long as its application sat in the store until the DVM
         * itself went away, and PMIX_PERSIST_APP and PMIX_PERSIST_PROC both
         * behaved as PMIX_PERSIST_INDEF. */
        if (!prte_data_server_expires_by(data->persistence, horizon)) {
            continue;
        }
        /* remove the object */
        pmix_pointer_array_set_item(&prte_data_store.store, data->index, NULL);
        PMIX_RELEASE(data);
    }

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
