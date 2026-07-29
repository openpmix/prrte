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

    /* unpack the proc whose data is to be purged - session
     * data is purged by providing a requestor whose rank
     * is wildcard */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &requestor, &count, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto done;
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
        /* remove the object */
        pmix_pointer_array_set_item(&prte_data_store.store, data->index, NULL);
        PMIX_RELEASE(data);
    }

    /* Drop any lookup this process left parked on the pending list. Those
     * requests outlived their requestor: a later publish would match one and
     * try to reply to a process that no longer exists, and until then the
     * request kept the (already purged) proc's keys alive. */
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
