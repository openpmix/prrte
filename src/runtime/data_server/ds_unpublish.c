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

pmix_status_t prte_ds_unpublish(pmix_proc_t *sender,
                                pmix_data_buffer_t *buffer,
                                pmix_data_buffer_t *answer)
{
    int32_t count;
    prte_data_object_t *data;
    pmix_status_t rc;
    int k;
    size_t n, ninfo;
    uint32_t i;
    char *str;
    prte_data_req_t rq;
    prte_info_item_t *ds1, *ds2;
    pmix_info_t *info;

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server got unpublish from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(sender));

    PMIX_CONSTRUCT(&rq, prte_data_req_t);
    memcpy(&rq.proxy, sender, sizeof(pmix_proc_t));

    /* unpack the requestor */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &rq.requestor, &count, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: unpublish data from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIX_NAME_PRINT(&rq.requestor));

    /* unpack the number of keys */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (0 == ninfo) {
        /* they forgot to send us the keys?? */
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }

    /* unpack the keys */
    for (n = 0; n < ninfo; n++) {
        count = 1;
        rc = PMIx_Data_unpack(NULL, buffer, &str, &count, PMIX_STRING);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&rq);
            return rc;
        }
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&rq.keys, str);
        free(str);
    }

    /* unpack the number of directives, if any */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        count = ninfo;
        rc = PMIx_Data_unpack(NULL, buffer, info, &count, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(info, ninfo);
            return rc;
        }
        /* scan the directives for things we care about */
        for (n = 0; n < ninfo; n++) {
            if (PMIx_Check_key(info[n].key, PMIX_USERID)) {
                rq.uid = info[n].value.data.uint32;
            } else if (PMIx_Check_key(info[n].key, PMIX_RANGE)) {
                rq.range = info[n].value.data.range;
            }
        }
        /* ignore anything else for now */
        PMIX_INFO_FREE(info, ninfo);
    }

    /* cycle across the provided keys */
    for (i = 0; NULL != rq.keys[i]; i++) {
        /* cycle across the stored data, looking for a match */
        for (k = 0; k < prte_data_store.store.size; k++) {
            data = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, k);
            if (NULL == data) {
                continue;
            }
            /* can only access data posted by the same user id */
            if (rq.uid != data->uid) {
                continue;
            }
            /* can only access data posted by the same process */
            if (!PMIX_CHECK_NSPACE(rq.requestor.nspace, data->owner.nspace) ||
                rq.requestor.rank != data->owner.rank) {
                continue;
            }
            /* check the range */
            if (PMIX_SUCCESS != prte_data_server_check_range(&rq, data)) {
                continue;
            }
            /* see if we have this key */
            PMIX_LIST_FOREACH_SAFE(ds1, ds2, &data->info, prte_info_item_t) {
                if (PMIx_Check_key(ds1->info.key, rq.keys[i])) {
                    /* found it -  remove that item */
                    pmix_list_remove_item(&data->info, &ds1->super);
                    PMIX_RELEASE(ds1);
                }
            }
            /* if all the data has been removed, then remove the object */
            if (0 == pmix_list_get_size(&data->info)) {
                pmix_pointer_array_set_item(&prte_data_store.store, data->index, NULL);
                PMIX_RELEASE(data);
            }
        }
    }
    PMIX_DESTRUCT(&rq);

    if (PMIX_SUCCESS == rc) {
        // send back an answer
        rc = PMIx_Data_pack(NULL, answer, &rc, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
        }
        PRTE_RML_SEND(rc, sender->rank, answer, PRTE_RML_TAG_DATA_CLIENT);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(answer);
        }
    }

    return rc;
}
