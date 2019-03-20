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

#include "orte_config.h"
#include "orte/constants.h"
#include "orte/types.h"

#include <string.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "opal/util/argv.h"
#include "opal/util/output.h"
#include "opal/class/opal_pointer_array.h"
#include "opal/dss/dss.h"
#include "opal/pmix/pmix-internal.h"

#include "orte/mca/errmgr/errmgr.h"
#include "orte/mca/rml/rml.h"
#include "orte/runtime/orte_globals.h"
#include "orte/util/name_fns.h"
#include "orte/runtime/orte_wait.h"
#include "orte/runtime/data_type_support/orte_dt_support.h"

#include "orte/runtime/orte_data_server.h"

/* define an object to hold data */
typedef struct {
    /* base object */
    opal_object_t super;
    /* index of this object in the storage array */
    orte_std_cntr_t index;
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
} orte_data_object_t;

static void construct(orte_data_object_t *ptr)
{
    ptr->index = -1;
    PMIX_PROC_CONSTRUCT(&ptr->owner);
    ptr->uid = UINT32_MAX;
    ptr->range = PMIX_RANGE_SESSION;
    ptr->persistence = PMIX_PERSIST_SESSION;
    ptr->info = NULL;
    ptr->ninfo = 0;
}

static void destruct(orte_data_object_t *ptr)
{
    if (NULL != ptr->info) {
        PMIX_INFO_FREE(ptr->info, ptr->ninfo);
    }
}

static OBJ_CLASS_INSTANCE(orte_data_object_t,
                          opal_object_t,
                          construct, destruct);

/* define a request object for delayed answers */
typedef struct {
    opal_list_item_t super;
    orte_process_name_t proxy;
    pmix_proc_t requestor;
    int room_number;
    uint32_t uid;
    pmix_data_range_t range;
    char **keys;
    opal_list_t answers;
} orte_data_req_t;
static void rqcon(orte_data_req_t *p)
{
    p->keys = NULL;
    OBJ_CONSTRUCT(&p->answers, opal_list_t);
}
static void rqdes(orte_data_req_t *p)
{
    opal_argv_free(p->keys);
    OPAL_LIST_DESTRUCT(&p->answers);
}
static OBJ_CLASS_INSTANCE(orte_data_req_t,
                          opal_list_item_t,
                          rqcon, rqdes);

/* local globals */
static opal_pointer_array_t orte_data_server_store;
static opal_list_t pending;
static bool initialized = false;
static int orte_data_server_output = -1;
static int orte_data_server_verbosity = -1;

int orte_data_server_init(void)
{
    int rc;

    if (initialized) {
        return ORTE_SUCCESS;
    }
    initialized = true;

    /* register a verbosity */
    orte_data_server_verbosity = -1;
    (void) mca_base_var_register ("orte", "orte", "data", "server_verbose",
                                  "Debug verbosity for ORTE data server",
                                  MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                  OPAL_INFO_LVL_9, MCA_BASE_VAR_SCOPE_ALL,
                                  &orte_data_server_verbosity);
    if (0 <= orte_data_server_verbosity) {
        orte_data_server_output = opal_output_open(NULL);
        opal_output_set_verbosity(orte_data_server_output,
                                  orte_data_server_verbosity);
    }

    OBJ_CONSTRUCT(&orte_data_server_store, opal_pointer_array_t);
    if (ORTE_SUCCESS != (rc = opal_pointer_array_init(&orte_data_server_store,
                                                      1,
                                                      INT_MAX,
                                                      1))) {
        ORTE_ERROR_LOG(rc);
        return rc;
    }

    OBJ_CONSTRUCT(&pending, opal_list_t);

    orte_rml.recv_buffer_nb(ORTE_NAME_WILDCARD,
                            ORTE_RML_TAG_DATA_SERVER,
                            ORTE_RML_PERSISTENT,
                            orte_data_server,
                            NULL);

    return ORTE_SUCCESS;
}

void orte_data_server_finalize(void)
{
    orte_std_cntr_t i;
    orte_data_object_t *data;

    if (!initialized) {
        return;
    }
    initialized = false;

    for (i=0; i < orte_data_server_store.size; i++) {
        if (NULL != (data = (orte_data_object_t*)opal_pointer_array_get_item(&orte_data_server_store, i))) {
            OBJ_RELEASE(data);
        }
    }
    OBJ_DESTRUCT(&orte_data_server_store);
    OPAL_LIST_DESTRUCT(&pending);
}

void orte_data_server(int status, orte_process_name_t* sender,
                      opal_buffer_t* buffer, orte_rml_tag_t tag,
                      void* cbdata)
{
    uint8_t command;
    orte_std_cntr_t count;
    orte_data_object_t *data;
    opal_byte_object_t bo, *boptr;
    opal_buffer_t *answer, *reply;
    int rc, k;
    uint32_t ninfo, i;
    char **keys = NULL, *str;
    bool wait = false;
    int room_number;
    uint32_t uid = UINT32_MAX;
    pmix_data_range_t range;
    orte_data_req_t *req, *rqnext;
    pmix_data_buffer_t pbkt;
    pmix_byte_object_t pbo;
    pmix_status_t ret;
    pmix_proc_t psender, requestor;
    opal_ds_info_t *rinfo;
    size_t n, nanswers;
    pmix_info_t *info;
    opal_list_t answers;

    opal_output_verbose(1, orte_data_server_output,
                        "%s data server got message from %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_NAME_PRINT(sender));

    /* unpack the room number of the caller's request */
    count = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &room_number, &count, OPAL_INT))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    /* unpack the command */
    count = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &command, &count, OPAL_UINT8))) {
        ORTE_ERROR_LOG(rc);
        return;
    }

    answer = OBJ_NEW(opal_buffer_t);
    /* pack the room number as this must lead any response */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(answer, &room_number, 1, OPAL_INT))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(answer);
        return;
    }
    /* and the command */
    if (ORTE_SUCCESS != (rc = opal_dss.pack(answer, &command, 1, OPAL_UINT8))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(answer);
        return;
    }

    /* unpack the byte object payload */
    count = 1;
    if (ORTE_SUCCESS != (rc = opal_dss.unpack(buffer, &boptr, &count, OPAL_BYTE_OBJECT))) {
        ORTE_ERROR_LOG(rc);
        goto SEND_ERROR;
    }

    /* load it into a pmix data buffer for processing */
    PMIX_DATA_BUFFER_LOAD(&pbkt, boptr->bytes, boptr->size);
    boptr->bytes = NULL;
    free(boptr);

    /* convert the sender */
    OPAL_PMIX_CONVERT_NAME(&psender, sender);

    switch(command) {
    case ORTE_PMIX_PUBLISH_CMD:
        data = OBJ_NEW(orte_data_object_t);

        /* unpack the publisher */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &data->owner, &count, PMIX_PROC))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            OBJ_RELEASE(data);
            rc = ORTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }

        opal_output_verbose(1, orte_data_server_output,
                            "%s data server: publishing data from %s:%d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            data->owner.nspace, data->owner.rank);

        /* unpack the number of infos they published */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &data->ninfo, &count, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            OBJ_RELEASE(data);
            rc = ORTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }

        /* if it isn't at least one, then that's an error */
        if (1 > data->ninfo) {
            ret = PMIX_ERR_BAD_PARAM;
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            OBJ_RELEASE(data);
            rc = ORTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }

        /* create the space */
        PMIX_INFO_CREATE(data->info, data->ninfo);

        /* unpack into it */
        count = data->ninfo;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, data->info, &count, PMIX_INFO))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            OBJ_RELEASE(data);
            rc = ORTE_ERR_UNPACK_FAILURE;
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
        data->index = opal_pointer_array_add(&orte_data_server_store, data);

        opal_output_verbose(1, orte_data_server_output,
                            "%s data server: checking for pending requests",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));

        /* check for pending requests that match this data */
        reply = NULL;
        OPAL_LIST_FOREACH_SAFE(req, rqnext, &pending, orte_data_req_t) {
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
                    opal_output_verbose(10, orte_data_server_output,
                                        "%s\tCHECKING %s TO %s",
                                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                        data->info[n].key, req->keys[i]);
                    if (0 == strncmp(data->info[n].key, req->keys[i], PMIX_MAX_KEYLEN)) {
                        opal_output_verbose(10, orte_data_server_output,
                                            "%s data server: packaging return",
                                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                        /* track this response */
                        opal_output_verbose(10, orte_data_server_output,
                                            "%s data server: adding %s data %s from %s:%d to response",
                                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), data->info[n].key,
                                            PMIx_Data_type_string(data->info[n].value.type),
                                            data->owner.nspace, data->owner.rank);
                        rinfo = OBJ_NEW(opal_ds_info_t);
                        memcpy(&rinfo->source, &data->owner, sizeof(pmix_proc_t));
                        rinfo->info = &data->info[n];
                        opal_list_append(&req->answers, &rinfo->super);
                        break;  // a key can only occur once
                    }
                }
            }
            if (0 < (n = opal_list_get_size(&req->answers))) {
                /* send it back to the requestor */
                opal_output_verbose(1, orte_data_server_output,
                                     "%s data server: returning data to %s:%d",
                                     ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                     req->requestor.nspace, req->requestor.rank);

                reply = OBJ_NEW(opal_buffer_t);
                /* start with their room number */
                if (ORTE_SUCCESS != (rc = opal_dss.pack(reply, &req->room_number, 1, OPAL_INT))) {
                    ORTE_ERROR_LOG(rc);
                    goto SEND_ERROR;
                }
                /* we are responding to a lookup cmd */
                command = ORTE_PMIX_LOOKUP_CMD;
                if (ORTE_SUCCESS != (rc = opal_dss.pack(reply, &command, 1, OPAL_UINT8))) {
                    ORTE_ERROR_LOG(rc);
                    goto SEND_ERROR;
                }
                /* if we found all of the requested keys, then indicate so */
                if (n == (size_t)opal_argv_count(req->keys)) {
                    i = ORTE_SUCCESS;
                } else {
                    i = ORTE_ERR_PARTIAL_SUCCESS;
                }
                /* return the status */
                if (ORTE_SUCCESS != (rc = opal_dss.pack(reply, &i, 1, OPAL_INT))) {
                    ORTE_ERROR_LOG(rc);
                    goto SEND_ERROR;
                }

                /* pack the rest into a pmix_data_buffer_t */
                PMIX_DATA_BUFFER_CONSTRUCT(&pbkt);

                /* pack the number of returned info's */
                if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, &n, 1, PMIX_SIZE))) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                    rc = ORTE_ERR_PACK_FAILURE;
                    goto SEND_ERROR;
                }
                /* loop thru and pack the individual responses - this is somewhat less
                 * efficient than packing an info array, but avoids another malloc
                 * operation just to assemble all the return values into a contiguous
                 * array */
                while (NULL != (rinfo = (opal_ds_info_t*)opal_list_remove_first(&req->answers))) {
                    /* pack the data owner */
                    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, &rinfo->source, 1, PMIX_PROC))) {
                        PMIX_ERROR_LOG(ret);
                        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                        rc = ORTE_ERR_PACK_FAILURE;
                        goto SEND_ERROR;
                    }
                    /* pack the data */
                    if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, rinfo->info, 1, PMIX_INFO))) {
                        PMIX_ERROR_LOG(ret);
                        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                        rc = ORTE_ERR_PACK_FAILURE;
                        goto SEND_ERROR;
                    }
                }
                OPAL_LIST_DESTRUCT(&req->answers);
                OBJ_CONSTRUCT(&req->answers, opal_list_t);

                /* unload the pmix buffer */
                PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
                bo.bytes = (uint8_t*)pbo.bytes;
                bo.size = pbo.size;

                /* pack it into our reply */
                boptr = &bo;
                if (ORTE_SUCCESS != (rc = opal_dss.pack(reply, &boptr, 1, OPAL_BYTE_OBJECT))) {
                    ORTE_ERROR_LOG(rc);
                    free(bo.bytes);
                    goto SEND_ERROR;
                }
                free(bo.bytes);
                if (0 > (rc = orte_rml.send_buffer_nb(&req->proxy, reply, ORTE_RML_TAG_DATA_CLIENT,
                                                      orte_rml_send_callback, NULL))) {
                    ORTE_ERROR_LOG(rc);
                    OBJ_RELEASE(reply);
                }
            }
        }

        /* tell the user it was wonderful... */
        rc = ORTE_SUCCESS;
        if (ORTE_SUCCESS != (rc = opal_dss.pack(answer, &rc, 1, OPAL_INT))) {
            ORTE_ERROR_LOG(rc);
            /* if we can't pack it, we probably can't pack the
             * rc value either, so just send whatever is there */
        }
        goto SEND_ANSWER;
        break;

    case ORTE_PMIX_LOOKUP_CMD:
        opal_output_verbose(1, orte_data_server_output,
                            "%s data server: lookup data from %s",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            ORTE_NAME_PRINT(sender));

        /* unpack the requestor */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &requestor, &count, PMIX_PROC))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = ORTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }

        /* unpack the number of keys */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &ninfo, &count, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = ORTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }
        if (0 == ninfo) {
            /* they forgot to send us the keys?? */
            ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
            rc = ORTE_ERR_BAD_PARAM;
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            goto SEND_ERROR;
        }

        /* unpack the keys */
        for (n=0; n < ninfo; n++) {
            count = 1;
            if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &str, &count, OPAL_STRING))) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                rc = ORTE_ERR_UNPACK_FAILURE;
                opal_argv_free(keys);
                goto SEND_ERROR;
            }
            opal_argv_append_nosize(&keys, str);
            free(str);
        }

        /* unpack the number of directives, if any */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &ninfo, &count, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = ORTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }
        if (0 < ninfo) {
            PMIX_INFO_CREATE(info, ninfo);
            count = ninfo;
            if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, info, &count, PMIX_INFO))) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                PMIX_INFO_FREE(info, ninfo);
                rc = ORTE_ERR_UNPACK_FAILURE;
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
        OBJ_CONSTRUCT(&answers, opal_list_t);

        for (i=0; NULL != keys[i]; i++) {
            opal_output_verbose(10, orte_data_server_output,
                                "%s data server: looking for %s",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), keys[i]);
            /* cycle across the stored data, looking for a match */
            for (k=0; k < orte_data_server_store.size; k++) {
                data = (orte_data_object_t*)opal_pointer_array_get_item(&orte_data_server_store, k);
                if (NULL == data) {
                    continue;
                }
                /* for security reasons, can only access data posted by the same user id */
                if (uid != data->uid) {
                    opal_output_verbose(10, orte_data_server_output,
                                        "%s\tMISMATCH UID %u %u",
                                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                        (unsigned)uid, (unsigned)data->uid);
                    continue;
                }
                /* if the published range is constrained to namespace, then only
                 * consider this data if the publisher is
                 * in the same namespace as the requestor */
                if (PMIX_RANGE_NAMESPACE == data->range) {
                    if (0 != strncmp(requestor.nspace, data->owner.nspace, PMIX_MAX_NSLEN)) {
                        opal_output_verbose(10, orte_data_server_output,
                                            "%s\tMISMATCH NSPACES %s %s",
                                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                            requestor.nspace, data->owner.nspace);
                        continue;
                    }
                }
                /* see if we have this key */
                for (n=0; n < data->ninfo; n++) {
                    opal_output_verbose(10, orte_data_server_output,
                                        "%s COMPARING %s %s",
                                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                        keys[i], data->info[n].key);
                    if (0 == strncmp(data->info[n].key, keys[i], PMIX_MAX_KEYLEN)) {
                        rinfo = OBJ_NEW(opal_ds_info_t);
                        memcpy(&rinfo->source, &data->owner, sizeof(pmix_proc_t));
                        rinfo->info = &data->info[n];
                        rinfo->persistence = data->persistence;
                        opal_list_append(&answers, &rinfo->super);
                        opal_output_verbose(1, orte_data_server_output,
                                            "%s data server: adding %s to data from %s:%d",
                                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), data->info[n].key,
                                            data->owner.nspace, data->owner.rank);
                    }
                }
            }  // loop over stored data
        }  // loop over keys

        if (0 < (nanswers = opal_list_get_size(&answers))) {
            /* pack the number of data items found */
            if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, &nanswers, 1, PMIX_SIZE))) {
                PMIX_ERROR_LOG(ret);
                rc = ORTE_ERR_PACK_FAILURE;
                OPAL_LIST_DESTRUCT(&answers);
                opal_argv_free(keys);
                goto SEND_ERROR;
            }
            /* loop thru and pack the individual responses - this is somewhat less
             * efficient than packing an info array, but avoids another malloc
             * operation just to assemble all the return values into a contiguous
             * array */
            OPAL_LIST_FOREACH(rinfo, &answers, opal_ds_info_t) {
                /* pack the data owner */
                if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, &rinfo->source, 1, PMIX_PROC))) {
                    PMIX_ERROR_LOG(ret);
                    rc = ORTE_ERR_PACK_FAILURE;
                    OPAL_LIST_DESTRUCT(&answers);
                    opal_argv_free(keys);
                    goto SEND_ERROR;
                }
                if (PMIX_SUCCESS != (ret = PMIx_Data_pack(&psender, &pbkt, rinfo->info, 1, PMIX_INFO))) {
                    PMIX_ERROR_LOG(ret);
                    rc = ORTE_ERR_PACK_FAILURE;
                    OPAL_LIST_DESTRUCT(&answers);
                    opal_argv_free(keys);
                    goto SEND_ERROR;
                }
                if (PMIX_PERSIST_FIRST_READ == rinfo->persistence) {
                    opal_output_verbose(1, orte_data_server_output,
                                        "%s REMOVING DATA FROM %s:%d FOR KEY %s",
                                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                                        rinfo->source.nspace, rinfo->source.rank,
                                        rinfo->info->key);
                    memset(rinfo->info->key, 0, PMIX_MAX_KEYLEN+1);
                }
            }
        }
        OPAL_LIST_DESTRUCT(&answers);

        if (nanswers == (size_t)opal_argv_count(keys)) {
            rc = ORTE_SUCCESS;
        } else {
            opal_output_verbose(1, orte_data_server_output,
                                "%s data server:lookup: at least some data not found %d vs %d",
                                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), (int)nanswers, (int)opal_argv_count(keys));

            /* if we were told to wait for the data, then queue this up
             * for later processing */
            if (wait) {
                opal_output_verbose(1, orte_data_server_output,
                                    "%s data server:lookup: pushing request to wait",
                                    ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
                OBJ_RELEASE(answer);
                req = OBJ_NEW(orte_data_req_t);
                req->room_number = room_number;
                req->proxy = *sender;
                memcpy(&req->requestor, &requestor, sizeof(pmix_proc_t));
                req->uid = uid;
                req->range = range;
                req->keys = keys;
                opal_list_append(&pending, &req->super);
                /* drop the partial response we have - we'll build it when everything
                 * becomes available */
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                return;
            }
            if (0 == nanswers) {
                /* nothing was found - indicate that situation */
                rc = ORTE_ERR_NOT_FOUND;
                opal_argv_free(keys);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                goto SEND_ERROR;
            } else {
                rc = ORTE_ERR_PARTIAL_SUCCESS;
            }
        }
        opal_argv_free(keys);
        opal_output_verbose(1, orte_data_server_output,
                            "%s data server:lookup: data found",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        /* pack the status */
        if (ORTE_SUCCESS != (rc = opal_dss.pack(answer, &rc, 1, OPAL_INT))) {
            ORTE_ERROR_LOG(rc);
            OBJ_RELEASE(answer);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            return;
        }
        /* unload the packed values */
        PMIX_DATA_BUFFER_UNLOAD(&pbkt, pbo.bytes, pbo.size);
        bo.bytes = (uint8_t*)pbo.bytes;
        bo.size = pbo.size;

        /* pack it into our reply */
        boptr = &bo;
        if (ORTE_SUCCESS != (rc = opal_dss.pack(answer, &boptr, 1, OPAL_BYTE_OBJECT))) {
            ORTE_ERROR_LOG(rc);
            free(bo.bytes);
            OBJ_RELEASE(answer);
            goto SEND_ERROR;
        }
        free(bo.bytes);

        goto SEND_ANSWER;
        break;

    case ORTE_PMIX_UNPUBLISH_CMD:
        /* unpack the requestor */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &requestor, &count, PMIX_PROC))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = ORTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }

        opal_output_verbose(1, orte_data_server_output,
                            "%s data server: unpublish data from %s:%d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            requestor.nspace, requestor.rank);

        /* unpack the number of keys */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &ninfo, &count, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = ORTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }
        if (0 == ninfo) {
            /* they forgot to send us the keys?? */
            ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
            rc = ORTE_ERR_BAD_PARAM;
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            goto SEND_ERROR;
        }

        /* unpack the keys */
        for (n=0; n < ninfo; n++) {
            count = 1;
            if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &str, &count, OPAL_STRING))) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                rc = ORTE_ERR_UNPACK_FAILURE;
                opal_argv_free(keys);
                goto SEND_ERROR;
            }
            opal_argv_append_nosize(&keys, str);
            free(str);
        }

        /* unpack the number of directives, if any */
        range = PMIX_RANGE_SESSION;  // default
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &ninfo, &count, PMIX_SIZE))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = ORTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }
        if (0 < ninfo) {
            PMIX_INFO_CREATE(info, ninfo);
            count = ninfo;
            if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, info, &count, PMIX_INFO))) {
                PMIX_ERROR_LOG(ret);
                PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
                PMIX_INFO_FREE(info, ninfo);
                rc = ORTE_ERR_UNPACK_FAILURE;
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
            for (k=0; k < orte_data_server_store.size; k++) {
                data = (orte_data_object_t*)opal_pointer_array_get_item(&orte_data_server_store, k);
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
                    opal_pointer_array_set_item(&orte_data_server_store, k, NULL);
                    OBJ_RELEASE(data);
                }
            }
        }
        opal_argv_free(keys);

        /* tell the sender this succeeded */
        ret = ORTE_SUCCESS;
        if (ORTE_SUCCESS != (rc = opal_dss.pack(answer, &ret, 1, OPAL_INT))) {
            ORTE_ERROR_LOG(rc);
        }
        goto SEND_ANSWER;
        break;

    case ORTE_PMIX_PURGE_PROC_CMD:
        /* unpack the proc whose data is to be purged - session
         * data is purged by providing a requestor whose rank
         * is wildcard */
        count = 1;
        if (PMIX_SUCCESS != (ret = PMIx_Data_unpack(&psender, &pbkt, &requestor, &count, PMIX_PROC))) {
            PMIX_ERROR_LOG(ret);
            PMIX_DATA_BUFFER_DESTRUCT(&pbkt);
            rc = ORTE_ERR_UNPACK_FAILURE;
            goto SEND_ERROR;
        }
        PMIX_DATA_BUFFER_DESTRUCT(&pbkt);

        opal_output_verbose(1, orte_data_server_output,
                            "%s data server: purge data from %s:%d",
                            ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                            requestor.nspace, requestor.rank);

        /* cycle across the stored data, looking for a match */
        for (k=0; k < orte_data_server_store.size; k++) {
            data = (orte_data_object_t*)opal_pointer_array_get_item(&orte_data_server_store, k);
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
            opal_pointer_array_set_item(&orte_data_server_store, k, NULL);
            OBJ_RELEASE(data);
        }
        /* no response is required */
        OBJ_RELEASE(answer);
        return;

    default:
        ORTE_ERROR_LOG(ORTE_ERR_BAD_PARAM);
        rc = ORTE_ERR_BAD_PARAM;
        break;
    }

  SEND_ERROR:
    opal_output_verbose(1, orte_data_server_output,
                        "%s data server: sending error %s",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
                        ORTE_ERROR_NAME(rc));
    /* pack the error code */
    if (ORTE_SUCCESS != (ret = opal_dss.pack(answer, &rc, 1, OPAL_INT))) {
        ORTE_ERROR_LOG(ret);
    }

  SEND_ANSWER:
    if (0 > (rc = orte_rml.send_buffer_nb(sender, answer, ORTE_RML_TAG_DATA_CLIENT,
                                          orte_rml_send_callback, NULL))) {
        ORTE_ERROR_LOG(rc);
        OBJ_RELEASE(answer);
    }
}
