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

pmix_status_t prte_ds_lookup(pmix_proc_t *sender, int room_number,
                             pmix_data_buffer_t *buffer,
                             pmix_data_buffer_t *answer)
{
    int32_t count;
    int i, k;
    size_t nanswers;
    pmix_status_t rc;
    pmix_proc_t requestor;
    size_t n, ninfo;
    char **keys = NULL, **cache = NULL;
    char *str;
    pmix_info_t *info;
    pmix_data_buffer_t pbkt;
    uint32_t uid = UINT32_MAX;
    bool wait = false;
    pmix_data_range_t range=PMIX_RANGE_UNDEF;
    prte_data_object_t *data;
    prte_ds_info_t *rinfo;
    prte_info_item_t *ds1, *ds2;
    pmix_list_t answers;
    bool found;
    prte_data_req_t *req, rq;
    pmix_byte_object_t pbo;

    /* unpack the requestor */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &requestor, &count, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

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
            PMIX_ARGV_FREE_COMPAT(keys);
            return rc;
        }
        PMIX_ARGV_APPEND_NOSIZE_COMPAT(&keys, str);
        free(str);
    }

    /* unpack the number of directives, if any */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &ninfo, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_ARGV_FREE_COMPAT(keys);
        return rc;
    }
    if (0 < ninfo) {
        PMIX_INFO_CREATE(info, ninfo);
        count = ninfo;
        rc = PMIx_Data_unpack(NULL, buffer, info, &count, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_INFO_FREE(info, ninfo);
            PMIX_ARGV_FREE_COMPAT(keys);
            return rc;
        }
        /* scan the directives for things we care about */
        for (n = 0; n < ninfo; n++) {
            if (PMIx_Check_key(info[n].key, PMIX_USERID)) {
                uid = info[n].value.data.uint32;
            } else if (PMIx_Check_key(info[n].key, PMIX_WAIT)) {
                /* flag that we wait until the data is present */
                wait = true;
            } else if (PMIx_Check_key(info[n].key, PMIX_RANGE)) {
                range = info[n].value.data.range;
            }
        }
        /* ignore anything else for now */
        PMIX_INFO_FREE(info, ninfo);
    }

    /* cycle across the provided keys */
    PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
    PMIX_CONSTRUCT(&answers, pmix_list_t);
    PMIX_CONSTRUCT(&rq, prte_data_req_t);
    memcpy(&rq.requestor, &requestor, sizeof(pmix_proc_t));
    memcpy(&rq.proxy, sender, sizeof(pmix_proc_t));

    for (i = 0; NULL != keys[i]; i++) {
        pmix_output_verbose(10, prte_data_store.output,
                            "%s data server: looking for %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), keys[i]);
        found = false;
        /* cycle across the stored data, looking for a match */
        for (k = 0; k < prte_data_store.store.size && !found; k++) {
            data = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, k);
            if (NULL == data) {
                continue;
            }
            /* for security reasons, can only access data posted by the same user id */
            if (uid != data->uid) {
                pmix_output_verbose(10, prte_data_store.output,
                                    "%s\tMISMATCH UID %u %u",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (unsigned) uid,
                                    (unsigned) data->uid);
                continue;
            }

            /* check the range */
            if (PMIX_SUCCESS != prte_data_server_check_range(&rq, data)) {
                continue;
            }
            /* see if we have this key */
            PMIX_LIST_FOREACH_SAFE(ds1, ds2, &data->info, prte_info_item_t) {
                pmix_output_verbose(10, prte_data_store.output,
                                    "%s COMPARING %s %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), keys[i],
                                    ds1->info.key);
                if (PMIx_Check_key(ds1->info.key, keys[i])) {
                    rinfo = PMIX_NEW(prte_ds_info_t);
                    memcpy(&rinfo->source, &data->owner, sizeof(pmix_proc_t));
                    PMIX_INFO_XFER(&rinfo->info, &ds1->info);
                    // check the persistence
                    pmix_output_verbose(1, prte_data_store.output,
                                        "%s data server: adding %s to data from %s",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), ds1->info.key,
                                        PRTE_NAME_PRINT(&data->owner));
                    if (PMIX_PERSIST_FIRST_READ == data->persistence) {
                        pmix_list_remove_item(&data->info, &ds1->super);
                        PMIX_RELEASE(ds1);
                    }
                    pmix_list_append(&answers, &rinfo->super);
                    // can only find it once - keys are required to be globally unique
                    // within a given range, and we checked the range above
                    found = true;
                    break;
                }
            }
        } // loop over stored data
        if (!found) {
            // cache the key
            PMIX_ARGV_APPEND_NOSIZE_COMPAT(&cache, keys[i]);
        }
    }     // loop over keys

    nanswers = pmix_list_get_size(&answers);
    rc = PMIX_ERR_NOT_FOUND;
    if (0 < nanswers) {
        /* pack the number of data items found */
        rc = PMIx_Data_pack(NULL, &pbkt, &nanswers, 1, PMIX_SIZE);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_LIST_DESTRUCT(&answers);
            PMIX_ARGV_FREE_COMPAT(keys);
            PMIX_ARGV_FREE_COMPAT(cache);
            return rc;
        }
        /* loop thru and pack the individual responses - this is somewhat less
         * efficient than packing an info array, but avoids another malloc
         * operation just to assemble all the return values into a contiguous
         * array */
        PMIX_LIST_FOREACH(rinfo, &answers, prte_ds_info_t)
        {
            /* pack the data owner */
            rc = PMIx_Data_pack(NULL, &pbkt, &rinfo->source, 1, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_LIST_DESTRUCT(&answers);
                PMIX_ARGV_FREE_COMPAT(keys);
                PMIX_ARGV_FREE_COMPAT(cache);
                return rc;
            }
            rc = PMIx_Data_pack(NULL, &pbkt, &rinfo->info, 1, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_LIST_DESTRUCT(&answers);
                PMIX_ARGV_FREE_COMPAT(keys);
                PMIX_ARGV_FREE_COMPAT(cache);
                return rc;
            }
        }
    }
    PMIX_LIST_DESTRUCT(&answers);

    i = PMIX_ARGV_COUNT_COMPAT(cache);
    if (0 < i) {
        if (wait) {
            pmix_output_verbose(1, prte_data_store.output,
                                "%s data server:lookup: at least some data not found %d vs %d",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), (int) nanswers,
                                (int) PMIX_ARGV_COUNT_COMPAT(keys));

            req = PMIX_NEW(prte_data_req_t);
            req->room_number = room_number;
            req->proxy = *sender;
            memcpy(&req->requestor, &requestor, sizeof(pmix_proc_t));
            req->uid = uid;
            req->range = range;
            req->keys = cache;
            cache = NULL;
            pmix_list_append(&prte_data_store.pending, &req->super);
            PMIX_ARGV_FREE_COMPAT(keys);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return PMIX_SUCCESS; // do not return an answer
        } else {
            PMIX_ARGV_FREE_COMPAT(cache);
            if (0 == nanswers) {
                /* nothing was found - indicate that situation */
                rc = PMIX_ERR_NOT_FOUND;
                PMIX_ARGV_FREE_COMPAT(keys);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                return rc;
            } else {
                rc = PMIX_ERR_PARTIAL_SUCCESS;
            }
        }
    }
    PMIX_ARGV_FREE_COMPAT(keys);

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server:lookup: data found - status %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PMIx_Error_string(rc));

    if (PMIX_SUCCESS == rc) {
        /* pack the status */
        rc = PMIx_Data_pack(NULL, answer, &rc, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return rc;
        }
        /* unload the packed values */
        rc = PMIx_Data_unload(&pbkt, &pbo);
        /* pack it into our reply */
        rc = PMIx_Data_pack(NULL, answer, &pbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
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
