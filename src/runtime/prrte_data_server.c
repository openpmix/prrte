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
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012-2016 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2018 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include <string.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/class/prrte_pointer_array.h"
#include "src/dss/dss.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/rml.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_wait.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

#include "src/runtime/prrte_data_server.h"

/* define an object to hold data */
typedef struct {
    /* base object */
    prrte_object_t super;
    /* index of this object in the storage array */
    prrte_std_cntr_t index;
    /* process that owns this data - only the
    * owner can remove it
    */
    pmix_proc_t owner;
    /* uid of the owner - helps control
     * access rights */
    uint32_t uid;
    /* characteristics */
    pmix_data_range_t range;
    pmix_persistence_t persistence;
    /* and the values themselves */
    pmix_info_t *info;
    size_t ninfo;
    /* the value itself */
} prrte_data_object_t;

static void construct(prrte_data_object_t *ptr)
{
    ptr->index = -1;
    PMIX_PROC_CONSTRUCT(&ptr->owner);
    ptr->uid = UINT32_MAX;
    ptr->range = PMIX_RANGE_SESSION;
    ptr->persistence = PMIX_PERSIST_SESSION;
    ptr->info = NULL;
    ptr->ninfo = 0;
}

static void destruct(prrte_data_object_t *ptr)
{
    if (NULL != ptr->info) {
        PMIX_INFO_FREE(ptr->info, ptr->ninfo);
    }
}

static PRRTE_CLASS_INSTANCE(prrte_data_object_t,
                          prrte_object_t,
                          construct, destruct);

/* define a request object for delayed answers */
typedef struct {
    prrte_list_item_t super;
    prrte_process_name_t proxy;
    pmix_proc_t requestor;
    int room_number;
    uint32_t uid;
    pmix_data_range_t range;
    char **keys;
    prrte_list_t answers;
} prrte_data_req_t;
static void rqcon(prrte_data_req_t *p)
{
    p->keys = NULL;
    PRRTE_CONSTRUCT(&p->answers, prrte_list_t);
}
static void rqdes(prrte_data_req_t *p)
{
    prrte_argv_free(p->keys);
    PRRTE_LIST_DESTRUCT(&p->answers);
}
static PRRTE_CLASS_INSTANCE(prrte_data_req_t,
                          prrte_list_item_t,
                          rqcon, rqdes);

/* local globals */
static prrte_pointer_array_t prrte_data_server_store;
static prrte_list_t pending;
static bool initialized = false;
static int prrte_data_server_output = -1;
static int prrte_data_server_verbosity = -1;

int prrte_data_server_init(void)
{
    int rc;

    if (initialized) {
        return PRRTE_SUCCESS;
    }
    initialized = true;

    /* register a verbosity */
    prrte_data_server_verbosity = -1;
    (void) prrte_mca_base_var_register ("prrte", "prrte", "data", "server_verbose",
                                  "Debug verbosity for PRRTE data server",
                                  PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_ALL,
                                  &prrte_data_server_verbosity);
    if (0 <= prrte_data_server_verbosity) {
        prrte_data_server_output = prrte_output_open(NULL);
        prrte_output_set_verbosity(prrte_data_server_output,
                                  prrte_data_server_verbosity);
    }

    PRRTE_CONSTRUCT(&prrte_data_server_store, prrte_pointer_array_t);
    if (PRRTE_SUCCESS != (rc = prrte_pointer_array_init(&prrte_data_server_store,
                                                      1,
                                                      INT_MAX,
                                                      1))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    PRRTE_CONSTRUCT(&pending, prrte_list_t);

    prrte_rml.recv_buffer_nb(PRRTE_NAME_WILDCARD,
                            PRRTE_RML_TAG_DATA_SERVER,
                            PRRTE_RML_PERSISTENT,
                            prrte_data_server,
                            NULL);

    return PRRTE_SUCCESS;
}

void prrte_data_server_finalize(void)
{
    prrte_std_cntr_t i;
    prrte_data_object_t *data;

    if (!initialized) {
        return;
    }
    initialized = false;

    for (i=0; i < prrte_data_server_store.size; i++) {
        if (NULL != (data = (prrte_data_object_t*)prrte_pointer_array_get_item(&prrte_data_server_store, i))) {
            PRRTE_RELEASE(data);
        }
    }
    PRRTE_DESTRUCT(&prrte_data_server_store);
    PRRTE_LIST_DESTRUCT(&pending);
}

void prrte_data_server(int status, prrte_process_name_t* sender,
                      prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                      void* cbdata)
{
    uint8_t command;
    prrte_std_cntr_t count;
    prrte_data_object_t *data;
    prrte_byte_object_t bo, *boptr;
    prrte_buffer_t *answer, *reply;
    int rc, k;
    uint32_t ninfo, i;
    char **keys = NULL, *str;
    bool wait = false;
    int room_number;
    uint32_t uid = UINT32_MAX;
    pmix_data_range_t range;
    prrte_data_req_t *req, *rqnext;
    pmix_data_buffer_t pbkt;
    pmix_byte_object_t pbo;
    pmix_status_t ret;
    pmix_proc_t psender, requestor;
    prrte_ds_info_t *rinfo;
    size_t n, nanswers;
    pmix_info_t *info;
    prrte_list_t answers;

    prrte_output_verbose(1, prrte_data_server_output,
                        "%s data server got message from %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(sender));

    /* unpack the room number of the caller's request */
    count = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &room_number, &count, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the command */
    count = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &command, &count, PRRTE_UINT8))) {
        PRRTE_ERROR_LOG(rc);
        return;
    }

    answer = PRRTE_NEW(prrte_buffer_t);
    /* pack the room number as this must lead any response */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(answer, &room_number, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(answer);
        return;
    }
    /* and the command */
    if (PRRTE_SUCCESS != (rc = prrte_dss.pack(answer, &command, 1, PRRTE_UINT8))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(answer);
        return;
    }

    /* unpack the byte object payload */
    count = 1;
    if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &boptr, &count, PRRTE_BYTE_OBJECT))) {
        PRRTE_ERROR_LOG(rc);
        goto SEND_ERROR;
    }

    /* load it into a pmix data buffer for processing */
    PMIX_DATA_BUFFER_LOAD(&pbkt, boptr->bytes, boptr->size);
    boptr->bytes = NULL;
    free(boptr);

    /* convert the sender */
    PRRTE_PMIX_CONVERT_NAME(&psender, sender);

    switch(command) {
    case PRRTE_PMIX_PUBLISH_CMD:
        data = PRRTE_NEW(prrte_data_object_t);

        /* unpack the publisher */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &data->owner, &count, PMIX_PROC))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PRRTE_RELEASE(data);
            rc = PRRTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }

        prrte_output_verbose(1, prrte_data_server_output,
                            "%s data server: publishing data from %s:%d",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            data->owner.nspace, data->owner.rank);

        /* unpack the number of infos they published */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &data->ninfo, &count, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PRRTE_RELEASE(data);
            rc = PRRTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }

        /* if it isn't at least one, then that's an error */
        if (1 > data->ninfo) {
            ret = PMIX_ERR_BAD_PARAM;
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PRRTE_RELEASE(data);
            rc = PRRTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }

        /* create the space */
        PMIX_INFO_CREATE(data->info, data->ninfo);

        /* unpack into it */
        count = data->ninfo;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, data->info, &count, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            PRRTE_RELEASE(data);
            rc = PRRTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

        /* check for directives */
        for (n=0; n < data->ninfo; n++) {
            if (0 == strcmp(data->info[n].key, PMIX_RANGE)) {
                data->range = data->info[n].value.data.range;
            } else if (0 == strcmp(data->info[n].key, PMIX_PERSISTENCE)) {
                data->persistence = data->info[n].value.data.persist;
            } else if (0 == strcmp(data->info[n].key, PMIX_USERID)) {
                data->uid = data->info[n].value.data.uint32;
            }
        }

        /* store this object */
        data->index = prrte_pointer_array_add(&prrte_data_server_store, data);

        prrte_output_verbose(1, prrte_data_server_output,
                            "%s data server: checking for pending requests",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

        /* check for pending requests that match this data */
        reply = NULL;
        PRRTE_LIST_FOREACH_SAFE(req, rqnext, &pending, prrte_data_req_t) {
            if (req->uid != data->uid) {
                continue;
            }
            /* if the published range is constrained to namespace, then only
             * consider this data if the publisher is
             * in the same namespace as the requestor */
            if (PMIX_RANGE_NAMESPACE == data->range) {
                if (0 != strncmp(req->requestor.nspace, data->owner.nspace, PMIX_MAX_NSLEN)) {
                    continue;
                }
            }
            for (i=0; NULL != req->keys[i]; i++) {
                /* cycle thru the data keys for matches */
                for (n=0; n < data->ninfo; n++) {
                    prrte_output_verbose(10, prrte_data_server_output,
                                        "%s\tCHECKING %s TO %s",
                                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                        data->info[n].key, req->keys[i]);
                    if (0 == strncmp(data->info[n].key, req->keys[i], PMIX_MAX_KEYLEN)) {
                        prrte_output_verbose(10, prrte_data_server_output,
                                            "%s data server: packaging return",
                                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
                        /* track this response */
                        prrte_output_verbose(10, prrte_data_server_output,
                                            "%s data server: adding %s data %s from %s:%d to response",
                                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), data->info[n].key,
                                            PMIx_Data_type_string(data->info[n].value.type),
                                            data->owner.nspace, data->owner.rank);
                        rinfo = PRRTE_NEW(prrte_ds_info_t);
                        memcpy(&rinfo->source, &data->owner, sizeof(pmix_proc_t));
                        rinfo->info = &data->info[n];
                        prrte_list_append(&req->answers, &rinfo->super);
                        break;  // a key can only occur once
                    }
                }
            }
            if (0 < (n = prrte_list_get_size(&req->answers))) {
                /* send it back to the requestor */
                prrte_output_verbose(1, prrte_data_server_output,
                                     "%s data server: returning data to %s:%d",
                                     PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                     req->requestor.nspace, req->requestor.rank);

                reply = PRRTE_NEW(prrte_buffer_t);
                /* start with their room number */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &req->room_number, 1, PRRTE_INT))) {
                    PRRTE_ERROR_LOG(rc);
                    goto SEND_ERROR;
                }
                /* we are responding to a lookup cmd */
                command = PRRTE_PMIX_LOOKUP_CMD;
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &command, 1, PRRTE_UINT8))) {
                    PRRTE_ERROR_LOG(rc);
                    goto SEND_ERROR;
                }
                /* if we found all of the requested keys, then indicate so */
                if (n == (size_t)prrte_argv_count(req->keys)) {
                    i = PRRTE_SUCCESS;
                } else {
                    i = PRRTE_ERR_PARTIAL_SUCCESS;
                }
                /* return the status */
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &i, 1, PRRTE_INT))) {
                    PRRTE_ERROR_LOG(rc);
                    goto SEND_ERROR;
                }

                /* pack the rest into a pmix_data_buffer_t */
                PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

                /* pack the number of returned info's */
                if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, &n, 1, PMIX_SIZE))) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                    rc = PRRTE_ERR_PACK_FAILURE;
                    goto SEND_ERROR;
                }
                /* loop thru and pack the individual responses - this is somewhat less
                 * efficient than packing an info array, but avoids another malloc
                 * operation just to assemble all the return values into a contiguous
                 * array */
                while (NULL != (rinfo = (prrte_ds_info_t*)prrte_list_remove_first(&req->answers))) {
                    /* pack the data owner */
                    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, &rinfo->source, 1, PMIX_PROC))) {
                        PMIX_ERROR_LOG(ret);
                        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                        rc = PRRTE_ERR_PACK_FAILURE;
                        goto SEND_ERROR;
                    }
                    /* pack the data */
                    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, rinfo->info, 1, PMIX_INFO))) {
                        PMIX_ERROR_LOG(ret);
                        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                        rc = PRRTE_ERR_PACK_FAILURE;
                        goto SEND_ERROR;
                    }
                }
                PRRTE_LIST_DESTRUCT(&req->answers);
                PRRTE_CONSTRUCT(&req->answers, prrte_list_t);

                /* unload the pmix buffer */
                PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
                bo.bytes = (uint8_t*)pbo.bytes;
                bo.size = pbo.size;

                /* pack it into our reply */
                boptr = &bo;
                if (PRRTE_SUCCESS != (rc = prrte_dss.pack(reply, &boptr, 1, PRRTE_BYTE_OBJECT))) {
                    PRRTE_ERROR_LOG(rc);
                    free(bo.bytes);
                    goto SEND_ERROR;
                }
                free(bo.bytes);
                if (0 > (rc = prrte_rml.send_buffer_nb(&req->proxy, reply, PRRTE_RML_TAG_DATA_CLIENT,
                                                      prrte_rml_send_callback, NULL))) {
                    PRRTE_ERROR_LOG(rc);
                    PRRTE_RELEASE(reply);
                }
            }
        }

        /* tell the user it was wonderful... */
        rc = PRRTE_SUCCESS;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(answer, &rc, 1, PRRTE_INT))) {
            PRRTE_ERROR_LOG(rc);
            /* if we can't pack it, we probably can't pack the
             * rc value either, so just send whatever is there */
        }
        goto SEND_ANSWER;
        break;

    case PRRTE_PMIX_LOOKUP_CMD:
        prrte_output_verbose(1, prrte_data_server_output,
                            "%s data server: lookup data from %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(sender));

        /* unpack the requestor */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &requestor, &count, PMIX_PROC))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = PRRTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }

        /* unpack the number of keys */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &ninfo, &count, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = PRRTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }
        if (0 == ninfo) {
            /* they forgot to send us the keys?? */
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
            rc = PRRTE_ERR_BAD_PARAM;
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            goto SEND_ERROR;
        }

        /* unpack the keys */
        for (n=0; n < ninfo; n++) {
            count = 1;
            if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &str, &count, PRRTE_STRING))) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                rc = PRRTE_ERR_UNPACK_FAILURE;
                prrte_argv_free(keys);
                goto SEND_ERROR;
            }
            prrte_argv_append_nosize(&keys, str);
            free(str);
        }

        /* unpack the number of directives, if any */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &ninfo, &count, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = PRRTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }
        if (0 < ninfo) {
            PMIX_INFO_CREATE(info, ninfo);
            count = ninfo;
            if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, info, &count, PMIX_INFO))) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                PMIX_INFO_FREE(info, ninfo);
                rc = PRRTE_ERR_UNPACK_FAILURE;
                goto SEND_ERROR;
            }
            /* scan the directives for things we care about */
            for (n=0; n < ninfo; n++) {
                if (0 == strncmp(info[n].key, PMIX_USERID, PMIX_MAX_KEYLEN)) {
                    uid = info[n].value.data.uint32;
                } else if (0 == strncmp(info[n].key, PMIX_WAIT, PMIX_MAX_KEYLEN)) {
                    /* flag that we wait until the data is present */
                    wait = true;
                } else if (0 == strcmp(info[n].key, PMIX_RANGE)) {
                    range = info[n].value.data.range;
                }
            }
            /* ignore anything else for now */
            PMIX_INFO_FREE(info, ninfo);
        }
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

        /* cycle across the provided keys */
        PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);
        PRRTE_CONSTRUCT(&answers, prrte_list_t);

        for (i=0; NULL != keys[i]; i++) {
            prrte_output_verbose(10, prrte_data_server_output,
                                "%s data server: looking for %s",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), keys[i]);
            /* cycle across the stored data, looking for a match */
            for (k=0; k < prrte_data_server_store.size; k++) {
                data = (prrte_data_object_t*)prrte_pointer_array_get_item(&prrte_data_server_store, k);
                if (NULL == data) {
                    continue;
                }
                /* for security reasons, can only access data posted by the same user id */
                if (uid != data->uid) {
                    prrte_output_verbose(10, prrte_data_server_output,
                                        "%s\tMISMATCH UID %u %u",
                                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                        (unsigned)uid, (unsigned)data->uid);
                    continue;
                }
                /* if the published range is constrained to namespace, then only
                 * consider this data if the publisher is
                 * in the same namespace as the requestor */
                if (PMIX_RANGE_NAMESPACE == data->range) {
                    if (0 != strncmp(requestor.nspace, data->owner.nspace, PMIX_MAX_NSLEN)) {
                        prrte_output_verbose(10, prrte_data_server_output,
                                            "%s\tMISMATCH NSPACES %s %s",
                                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                            requestor.nspace, data->owner.nspace);
                        continue;
                    }
                }
                /* see if we have this key */
                for (n=0; n < data->ninfo; n++) {
                    prrte_output_verbose(10, prrte_data_server_output,
                                        "%s COMPARING %s %s",
                                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                        keys[i], data->info[n].key);
                    if (0 == strncmp(data->info[n].key, keys[i], PMIX_MAX_KEYLEN)) {
                        rinfo = PRRTE_NEW(prrte_ds_info_t);
                        memcpy(&rinfo->source, &data->owner, sizeof(pmix_proc_t));
                        rinfo->info = &data->info[n];
                        rinfo->persistence = data->persistence;
                        prrte_list_append(&answers, &rinfo->super);
                        prrte_output_verbose(1, prrte_data_server_output,
                                            "%s data server: adding %s to data from %s:%d",
                                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), data->info[n].key,
                                            data->owner.nspace, data->owner.rank);
                    }
                }
            }  // loop over stored data
        }  // loop over keys

        if (0 < (nanswers = prrte_list_get_size(&answers))) {
            /* pack the number of data items found */
            if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, &nanswers, 1, PMIX_SIZE))) {
                PMIX_ERROR_LOG(ret);
                rc = PRRTE_ERR_PACK_FAILURE;
                PRRTE_LIST_DESTRUCT(&answers);
                prrte_argv_free(keys);
                goto SEND_ERROR;
            }
            /* loop thru and pack the individual responses - this is somewhat less
             * efficient than packing an info array, but avoids another malloc
             * operation just to assemble all the return values into a contiguous
             * array */
            PRRTE_LIST_FOREACH(rinfo, &answers, prrte_ds_info_t) {
                /* pack the data owner */
                if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, &rinfo->source, 1, PMIX_PROC))) {
                    PMIX_ERROR_LOG(ret);
                    rc = PRRTE_ERR_PACK_FAILURE;
                    PRRTE_LIST_DESTRUCT(&answers);
                    prrte_argv_free(keys);
                    goto SEND_ERROR;
                }
                if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, rinfo->info, 1, PMIX_INFO))) {
                    PMIX_ERROR_LOG(ret);
                    rc = PRRTE_ERR_PACK_FAILURE;
                    PRRTE_LIST_DESTRUCT(&answers);
                    prrte_argv_free(keys);
                    goto SEND_ERROR;
                }
                if (PMIX_PERSIST_FIRST_READ == rinfo->persistence) {
                    prrte_output_verbose(1, prrte_data_server_output,
                                        "%s REMOVING DATA FROM %s:%d FOR KEY %s",
                                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                        rinfo->source.nspace, rinfo->source.rank,
                                        rinfo->info->key);
                    memset(rinfo->info->key, 0, PMIX_MAX_KEYLEN+1);
                }
            }
        }
        PRRTE_LIST_DESTRUCT(&answers);

        if (nanswers == (size_t)prrte_argv_count(keys)) {
            rc = PRRTE_SUCCESS;
        } else {
            prrte_output_verbose(1, prrte_data_server_output,
                                "%s data server:lookup: at least some data not found %d vs %d",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), (int)nanswers, (int)prrte_argv_count(keys));

            /* if we were told to wait for the data, then queue this up
             * for later processing */
            if (wait) {
                prrte_output_verbose(1, prrte_data_server_output,
                                    "%s data server:lookup: pushing request to wait",
                                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
                PRRTE_RELEASE(answer);
                req = PRRTE_NEW(prrte_data_req_t);
                req->room_number = room_number;
                req->proxy = *sender;
                memcpy(&req->requestor, &requestor, sizeof(pmix_proc_t));
                req->uid = uid;
                req->range = range;
                req->keys = keys;
                prrte_list_append(&pending, &req->super);
                /* drop the partial response we have - we'll build it when everything
                 * becomes available */
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                return;
            }
            if (0 == nanswers) {
                /* nothing was found - indicate that situation */
                rc = PRRTE_ERR_NOT_FOUND;
                prrte_argv_free(keys);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                goto SEND_ERROR;
            } else {
                rc = PRRTE_ERR_PARTIAL_SUCCESS;
            }
        }
        prrte_argv_free(keys);
        prrte_output_verbose(1, prrte_data_server_output,
                            "%s data server:lookup: data found",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        /* pack the status */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(answer, &rc, 1, PRRTE_INT))) {
            PRRTE_ERROR_LOG(rc);
            PRRTE_RELEASE(answer);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return;
        }
        /* unload the packed values */
        PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
        bo.bytes = (uint8_t*)pbo.bytes;
        bo.size = pbo.size;

        /* pack it into our reply */
        boptr = &bo;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(answer, &boptr, 1, PRRTE_BYTE_OBJECT))) {
            PRRTE_ERROR_LOG(rc);
            free(bo.bytes);
            PRRTE_RELEASE(answer);
            goto SEND_ERROR;
        }
        free(bo.bytes);

        goto SEND_ANSWER;
        break;

    case PRRTE_PMIX_UNPUBLISH_CMD:
        /* unpack the requestor */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &requestor, &count, PMIX_PROC))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = PRRTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }

        prrte_output_verbose(1, prrte_data_server_output,
                            "%s data server: unpublish data from %s:%d",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            requestor.nspace, requestor.rank);

        /* unpack the number of keys */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &ninfo, &count, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = PRRTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }
        if (0 == ninfo) {
            /* they forgot to send us the keys?? */
            PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
            rc = PRRTE_ERR_BAD_PARAM;
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            goto SEND_ERROR;
        }

        /* unpack the keys */
        for (n=0; n < ninfo; n++) {
            count = 1;
            if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &str, &count, PRRTE_STRING))) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                rc = PRRTE_ERR_UNPACK_FAILURE;
                prrte_argv_free(keys);
                goto SEND_ERROR;
            }
            prrte_argv_append_nosize(&keys, str);
            free(str);
        }

        /* unpack the number of directives, if any */
        range = PMIX_RANGE_SESSION;  // default
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &ninfo, &count, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = PRRTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }
        if (0 < ninfo) {
            PMIX_INFO_CREATE(info, ninfo);
            count = ninfo;
            if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, info, &count, PMIX_INFO))) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                PMIX_INFO_FREE(info, ninfo);
                rc = PRRTE_ERR_UNPACK_FAILURE;
                goto SEND_ERROR;
            }
            /* scan the directives for things we care about */
            for (n=0; n < ninfo; n++) {
                if (0 == strncmp(info[n].key, PMIX_USERID, PMIX_MAX_KEYLEN)) {
                    uid = info[n].value.data.uint32;
                } else if (0 == strncmp(info[n].key, PMIX_RANGE, PMIX_MAX_KEYLEN)) {
                    range = info[n].value.data.range;
                }
            }
            /* ignore anything else for now */
            PMIX_INFO_FREE(info, ninfo);
        }
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

        /* cycle across the provided keys */
        for (i=0; NULL != keys[i]; i++) {
            /* cycle across the stored data, looking for a match */
            for (k=0; k < prrte_data_server_store.size; k++) {
                data = (prrte_data_object_t*)prrte_pointer_array_get_item(&prrte_data_server_store, k);
                if (NULL == data) {
                    continue;
                }
                /* can only access data posted by the same user id */
                if (uid != data->uid) {
                    continue;
                }
                /* can only access data posted by the same process */
                if (0 != strncmp(requestor.nspace, data->owner.nspace, PMIX_MAX_NSLEN) ||
                    requestor.rank != data->owner.rank) {
                    continue;
                }
                /* can only access data posted for the same range */
                if (range != data->range) {
                    continue;
                }
                /* see if we have this key */
                nanswers = 0;
                for (n=0; n < data->ninfo; n++) {
                    if (0 == strlen(data->info[n].key)) {
                        ++nanswers;
                        continue;
                    }
                    if (0 == strncmp(data->info[n].key, keys[i], PMIX_MAX_KEYLEN)) {
                        /* found it -  delete the object from the data store */
                        memset(data->info[n].key, 0, PMIX_MAX_KEYLEN+1);
                        ++nanswers;
                    }
                }
                /* if all the data has been removed, then remove the object */
                if (nanswers == data->ninfo) {
                    prrte_pointer_array_set_item(&prrte_data_server_store, k, NULL);
                    PRRTE_RELEASE(data);
                }
            }
        }
        prrte_argv_free(keys);

        /* tell the sender this succeeded */
        ret = PRRTE_SUCCESS;
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(answer, &ret, 1, PRRTE_INT))) {
            PRRTE_ERROR_LOG(rc);
        }
        goto SEND_ANSWER;
        break;

    case PRRTE_PMIX_PURGE_PROC_CMD:
        /* unpack the proc whose data is to be purged - session
         * data is purged by providing a requestor whose rank
         * is wildcard */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &requestor, &count, PMIX_PROC))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = PRRTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

        prrte_output_verbose(1, prrte_data_server_output,
                            "%s data server: purge data from %s:%d",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            requestor.nspace, requestor.rank);

        /* cycle across the stored data, looking for a match */
        for (k=0; k < prrte_data_server_store.size; k++) {
            data = (prrte_data_object_t*)prrte_pointer_array_get_item(&prrte_data_server_store, k);
            if (NULL == data) {
                continue;
            }
            /* check if data posted by the specified process */
            if (0 != strncmp(requestor.nspace, data->owner.nspace, PMIX_MAX_NSLEN) ||
                (PMIX_RANK_WILDCARD != requestor.rank && requestor.rank != data->owner.rank)) {
                continue;
            }
            /* check persistence - if it is intended to persist beyond the
             * proc itself, then we only delete it if rank=wildcard*/
            if ((data->persistence == PMIX_PERSIST_APP ||
                 data->persistence == PMIX_PERSIST_SESSION) &&
                PMIX_RANK_WILDCARD != requestor.rank) {
                continue;
            }
            /* remove the object */
            prrte_pointer_array_set_item(&prrte_data_server_store, k, NULL);
            PRRTE_RELEASE(data);
        }
        /* no response is required */
        PRRTE_RELEASE(answer);
        return;

    default:
        PRRTE_ERROR_LOG(PRRTE_ERR_BAD_PARAM);
        rc = PRRTE_ERR_BAD_PARAM;
        break;
    }

  SEND_ERROR:
    prrte_output_verbose(1, prrte_data_server_output,
                        "%s data server: sending error %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_ERROR_NAME(rc));
    /* pack the error code */
    if (PRRTE_SUCCESS != (ret = prrte_dss.pack(answer, &rc, 1, PRRTE_INT))) {
        PRRTE_ERROR_LOG(ret);
    }

  SEND_ANSWER:
    if (0 > (rc = prrte_rml.send_buffer_nb(sender, answer, PRRTE_RML_TAG_DATA_CLIENT,
                                          prrte_rml_send_callback, NULL))) {
        PRRTE_ERROR_LOG(rc);
        PRRTE_RELEASE(answer);
    }
}
