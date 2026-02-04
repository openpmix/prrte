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

pmix_status_t prte_ds_publish(pmix_proc_t *sender,
                              pmix_data_buffer_t *buffer,
                              pmix_data_buffer_t *answer)
{
    uint8_t command;
    int32_t count;
    prte_data_object_t *data;
    pmix_data_buffer_t *reply;
    int rc;
    size_t ninfo;
    uint32_t i;
    bool complete_resolved, found;
    prte_data_req_t *req, *rqnext;
    pmix_data_buffer_t pbkt;
    pmix_byte_object_t pbo;
    pmix_status_t ret;
    prte_info_item_t *ds1, *ds2, *ds3;
    size_t n;
    pmix_info_t *info;
    char **cache;
    pmix_list_t answers;

    data = PMIX_NEW(prte_data_object_t);
    memcpy(&data->proxy, sender, sizeof(pmix_proc_t));

    /* unpack the publisher */
    count = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &data->owner, &count, PMIX_PROC);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(data);
        return ret;
    }

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: publishing data from %s:%d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), data->owner.nspace,
                        data->owner.rank);

    /* unpack the number of infos and directives they sent */
    count = 1;
    ret = PMIx_Data_unpack(NULL, buffer, &ninfo, &count, PMIX_SIZE);
    if (PMIX_SUCCESS != ret) {
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(data);
        return ret;
    }

    /* if it isn't at least one, then that's an error */
    if (1 > ninfo) {
        ret = PMIX_ERR_BAD_PARAM;
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(data);
        rc = PRTE_ERR_UNPACK_FAILURE;
        return rc;
    }

    /* create the space */
    PMIX_INFO_CREATE(info, ninfo);

    /* unpack into it */
    count = ninfo;
    if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(NULL, buffer, info, &count, PMIX_INFO))) {
        PMIX_ERROR_LOG(ret);
        PMIX_RELEASE(data);
        PMIX_INFO_FREE(info, ninfo);
        rc = PRTE_ERR_UNPACK_FAILURE;
        return rc;
    }

    /* check for directives */
    for (n = 0; n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, PMIX_RANGE)) {
            data->range = info[n].value.data.range;
        } else if (PMIx_Check_key(info[n].key, PMIX_PERSISTENCE)) {
            data->persistence = info[n].value.data.persist;
        } else if (PMIx_Check_key(info[n].key, PMIX_USERID)) {
            data->uid = info[n].value.data.uint32;
        } else {
            /* add it to the list of data */
            ds1 = PMIX_NEW(prte_info_item_t);
            PMIX_INFO_XFER(&ds1->info, &info[n]);
            pmix_list_append(&data->info, &ds1->super);
        }
    }

    // add this data to our store
    data->index = pmix_pointer_array_add(&prte_data_store.store, data);

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server: checking for pending requests",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* check for pending requests that match this data */
    reply = NULL;
    rc = PRTE_SUCCESS;
    PMIX_LIST_FOREACH_SAFE(req, rqnext, &prte_data_store.pending, prte_data_req_t)
    {
        if (req->uid != data->uid) {
            continue;
        }
        /* check the range */
        if (PMIX_SUCCESS != prte_data_server_check_range(req, data)) {
            continue;
        }

        complete_resolved = false;
        cache = NULL;
        PMIX_CONSTRUCT(&answers, pmix_list_t);

        for (i = 0; NULL != req->keys[i]; i++) {
            /* cycle thru the data keys for matches */
            found = false;
            PMIX_LIST_FOREACH_SAFE(ds1, ds2, &data->info, prte_info_item_t) {
                pmix_output_verbose(10, prte_data_store.output,
                                    "%s\tCHECKING %s TO %s",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    ds1->info.key, req->keys[i]);

                if (PMIx_Check_key(ds1->info.key, req->keys[i])) {
                    pmix_output_verbose(10, prte_data_store.output,
                                        "%s data server: packaging return",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                    /* track this response */
                    pmix_output_verbose(
                        10, prte_data_store.output,
                        "%s data server: adding %s data %s from %s:%d to response",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), ds1->info.key,
                        PMIx_Data_type_string(ds1->info.value.type), data->owner.nspace,
                        data->owner.rank);
                    ds3 = PMIX_NEW(prte_info_item_t);
                    PMIX_INFO_XFER(&ds3->info, &ds1->info);
                    pmix_list_append(&answers, &ds3->super);
                    // if the persistence is "first read", then remove this info
                    if (PMIX_PERSIST_FIRST_READ == data->persistence) {
                        pmix_list_remove_item(&data->info, &ds1->super);
                        PMIX_RELEASE(ds1);
                    }
                    found = true;
                    break; // a key can only occur once
                }
            }
            if (!found) {
                PMIx_Argv_append_nosize(&cache, req->keys[i]);
            }
        }
        // update the keys to remove all that have been resolved
        if (0 < PMIx_Argv_count(cache)) {
            PMIx_Argv_free(req->keys);
            req->keys = cache;
        } else {
            // if no keys are in the cache, then all keys were resolved
            complete_resolved = true;
        }

        n = pmix_list_get_size(&answers);
        if (0 == n) {
            PMIX_LIST_DESTRUCT(&answers);
            continue;
        }


        /* send the answers back to the requestor */
        pmix_output_verbose(1, prte_data_store.output,
                            "%s data server:publish returning %lu data to %s:%d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            (unsigned long)n,
                            req->requestor.nspace,
                            req->requestor.rank);

        PMIX_DATA_BUFFER_CREATE(reply);
        /* start with their room number */
        rc = PMIx_Data_pack(NULL, reply, &req->room_number, 1, PMIX_INT);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return rc;
        }
        /* we are responding to a lookup cmd */
        command = PRTE_PMIX_LOOKUP_CMD;
        rc = PMIx_Data_pack(NULL, reply, &command, 1, PMIX_UINT8);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return rc;
        }
        /* if we found all of the requested keys, then indicate so */
        if (n == (size_t) PMIx_Argv_count(req->keys)) {
            rc = PMIX_SUCCESS;
        } else {
            rc = PMIX_ERR_PARTIAL_SUCCESS;
        }
        /* return the status */
        rc = PMIx_Data_pack(NULL, reply, &rc, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            return rc;
        }

        /* pack the rest into a pmix_data_buffer_t */
        PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

        /* pack the number of returned info's */
        if (PMIX_SUCCESS != (ret = PMIx_Data_pack(NULL, &pbkt, &n, 1, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = PRTE_ERR_PACK_FAILURE;
            PMIX_DATA_BUFFER_RELEASE(reply);
            return rc;
        }
        /* loop thru and pack the individual responses - this is somewhat less
         * efficient than packing an info array, but avoids another malloc
         * operation just to assemble all the return values into a contiguous
         * array */
        while (NULL != (ds3 = (prte_info_item_t *) pmix_list_remove_first(&answers))) {
            /* pack the data owner */
            ret = PMIx_Data_pack(NULL, &pbkt, &data->owner, 1, PMIX_PROC);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                rc = PRTE_ERR_PACK_FAILURE;
                PMIX_DATA_BUFFER_RELEASE(reply);
                return rc;
            }
            /* pack the data */
            ret = PMIx_Data_pack(NULL, &pbkt, &ds3->info, 1, PMIX_INFO);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                rc = PRTE_ERR_PACK_FAILURE;
                PMIX_DATA_BUFFER_RELEASE(reply);
                return rc;
            }
        }
        PMIX_LIST_DESTRUCT(&answers);

        /* unload the pmix buffer */
        rc = PMIx_Data_unload(&pbkt, &pbo);

        /* pack it into our reply */
        rc = PMIx_Data_pack(NULL, reply, &pbo, 1, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
            PMIX_RELEASE(req);
            return rc;
        }
        PRTE_RML_SEND(rc, req->proxy.rank, reply, PRTE_RML_TAG_DATA_CLIENT);
        if (PRTE_SUCCESS != rc) {
            PRTE_ERROR_LOG(rc);
            PMIX_DATA_BUFFER_RELEASE(reply);
        }
        if (0 == pmix_list_get_size(&data->info)) {
            // all the data was removed, so we no longer need this entry
            pmix_pointer_array_set_item(&prte_data_store.store, data->index, NULL);
            PMIX_RELEASE(data);
            data = NULL;
        }
        if (complete_resolved) {
            // completely resolved this pending request, so remove it
            pmix_list_remove_item(&prte_data_store.pending, &req->super);
            PMIX_RELEASE(req);
        }
        if (NULL == data) {
            break;
        }
    }

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
