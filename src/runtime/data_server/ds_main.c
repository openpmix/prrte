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


// globals
prte_data_store_t prte_data_store = {
    .store = PMIX_POINTER_ARRAY_STATIC_INIT,
    .pending = PMIX_LIST_STATIC_INIT,
    .output = -1,
    .verbosity = 0
};


/* locals */
static bool initialized = false;

int prte_data_server_init(void)
{
    pmix_status_t rc;

    if (initialized) {
        return PRTE_SUCCESS;
    }
    initialized = true;

    /* register a verbosity */
    prte_data_store.verbosity = -1;
    (void) pmix_mca_base_var_register("prte", "prte", "data", "server_verbose",
                                      "Debug verbosity for PRTE data server",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &prte_data_store.verbosity);
    if (0 <= prte_data_store.verbosity) {
        prte_data_store.output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_data_store.output, prte_data_store.verbosity);
    }

    PMIX_CONSTRUCT(&prte_data_store.store, pmix_pointer_array_t);
    if (PMIX_SUCCESS != (rc = pmix_pointer_array_init(&prte_data_store.store, 1, INT_MAX, 1))) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    PMIX_CONSTRUCT(&prte_data_store.pending, pmix_list_t);

    PRTE_RML_RECV(PRTE_NAME_WILDCARD, PRTE_RML_TAG_DATA_SERVER,
                  PRTE_RML_PERSISTENT, prte_data_server, NULL);

    return PRTE_SUCCESS;
}

void prte_data_server_finalize(void)
{
    int32_t i;
    prte_data_object_t *data;

    if (!initialized) {
        return;
    }
    initialized = false;

    for (i = 0; i < prte_data_store.store.size; i++) {
        data = (prte_data_object_t *) pmix_pointer_array_get_item(&prte_data_store.store, i);
        if (NULL != data) {
            PMIX_RELEASE(data);
        }
    }
    PMIX_DESTRUCT(&prte_data_store.store);
    PMIX_LIST_DESTRUCT(&prte_data_store.pending);
}

void prte_data_server(int status, pmix_proc_t *sender,
                      pmix_data_buffer_t *buffer,
                      prte_rml_tag_t tag, void *cbdata)
{
    uint8_t command;
    int32_t count;
    pmix_data_buffer_t *answer;
    pmix_status_t rc;
    int room_number;
    PRTE_HIDE_UNUSED_PARAMS(status, tag, cbdata);

    pmix_output_verbose(1, prte_data_store.output,
                        "%s data server got message from %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(sender));

    /* unpack the room number of the caller's request */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &room_number, &count, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    /* unpack the command */
    count = 1;
    rc = PMIx_Data_unpack(NULL, buffer, &command, &count, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return;
    }

    PMIX_DATA_BUFFER_CREATE(answer);
    /* pack the room number as this must lead any response */
    rc = PMIx_Data_pack(NULL, answer, &room_number, 1, PMIX_INT);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return;
    }
    /* and the command */
    rc = PMIx_Data_pack(NULL, answer, &command, 1, PMIX_UINT8);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_DATA_BUFFER_RELEASE(answer);
        return;
    }

    switch (command) {
        case PRTE_PMIX_PUBLISH_CMD:
            rc = prte_ds_publish(sender, buffer, answer);
            break;

        case PRTE_PMIX_LOOKUP_CMD:
            pmix_output_verbose(1, prte_data_store.output,
                                "%s data server: lookup data from %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                PRTE_NAME_PRINT(sender));
            rc = prte_ds_lookup(sender, room_number,
                                buffer, answer);
            break;

        case PRTE_PMIX_UNPUBLISH_CMD:
            rc = prte_ds_unpublish(sender, buffer, answer);
            break;

        case PRTE_PMIX_PURGE_PROC_CMD:
            prte_ds_purge(sender, buffer, answer);
            return;

        default:
            PRTE_ERROR_LOG(PRTE_ERR_BAD_PARAM);
            rc = PRTE_ERR_BAD_PARAM;
            break;
    }

    if (PMIX_SUCCESS != rc) {
        pmix_output_verbose(1, prte_data_store.output,
                            "%s data server: sending error %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            PRTE_ERROR_NAME(rc));
        /* pack the error code */
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

}

pmix_status_t prte_data_server_check_range(prte_data_req_t *req,
                                           prte_data_object_t *data)
{
    // we automatically accept session and global ranges
    if (PMIX_RANGE_SESSION == data->range ||
        PMIX_RANGE_GLOBAL == data->range ||
        PMIX_RANGE_UNDEF == data->range) {
        return PMIX_SUCCESS;
    }

    if (PMIX_RANGE_NAMESPACE == data->range) {
        if (PMIX_CHECK_NSPACE(req->requestor.nspace, data->owner.nspace)) {
            pmix_output_verbose(10, prte_data_store.output,
                                "%s\tMATCH NSPACES %s %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                req->requestor.nspace,
                                data->owner.nspace);
            return PMIX_SUCCESS;
        }
    }
    if (PMIX_RANGE_LOCAL == data->range) {
        // the sender is the requestor's daemon, so see if
        // that matches the published data's proxy
        if (PMIX_CHECK_PROCID(&data->proxy, &req->proxy)) {
            pmix_output_verbose(10, prte_data_store.output,
                                "%s\tMATCH LOCATION %s %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                PMIX_NAME_PRINT(&data->proxy),
                                PMIX_NAME_PRINT(&req->proxy));
            return PMIX_SUCCESS;
        }
    }
    if (PMIX_RANGE_PROC_LOCAL == data->range) {
        // the requestor must be the same as the owner
        if (PMIX_CHECK_PROCID(&data->owner, &req->requestor)) {
            pmix_output_verbose(10, prte_data_store.output,
                                "%s\tMATCH LOCAL %s %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                PMIX_NAME_PRINT(&data->owner),
                                PMIX_NAME_PRINT(&req->requestor));
            return PMIX_SUCCESS;
        }
    }

    if (PMIX_RANGE_CUSTOM == data->range) {
        // requestor must be on the list of allowed accessors

    }

    if (PMIX_RANGE_RM == data->range) {
        // the requestor must be from the host - which means
        // the nspace of the requestor must match that of
        // the host's server, which is my own
        if (PMIX_CHECK_NSPACE(req->requestor.nspace, PRTE_PROC_MY_NAME->nspace)) {
            pmix_output_verbose(10, prte_data_store.output,
                                "%s\tMATCH RM %s %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                req->requestor.nspace,
                                PRTE_PROC_MY_NAME->nspace);
            return PMIX_SUCCESS;
        }
    }

    // no matches
    return PMIX_ERROR;
}

// CLASS INSTANCE
static void construct(prte_data_object_t *ptr)
{
    ptr->index = -1;
    PMIX_PROC_CONSTRUCT(&ptr->owner);
    ptr->uid = UINT32_MAX;
    ptr->range = PMIX_RANGE_SESSION;
    ptr->persistence = PMIX_PERSIST_SESSION;
    PMIX_CONSTRUCT(&ptr->info, pmix_list_t);
}

static void destruct(prte_data_object_t *ptr)
{
    PMIX_LIST_DESTRUCT(&ptr->info);
}

PMIX_CLASS_INSTANCE(prte_data_object_t,
                    pmix_object_t,
                    construct, destruct);


static void rqcon(prte_data_req_t *p)
{
    p->keys = NULL;
    p->uid = UINT32_MAX;
    p->range = PMIX_RANGE_UNDEF;
}
static void rqdes(prte_data_req_t *p)
{
    PMIX_ARGV_FREE_COMPAT(p->keys);
}
PMIX_CLASS_INSTANCE(prte_data_req_t,
                    pmix_list_item_t,
                    rqcon, rqdes);


PMIX_CLASS_INSTANCE(prte_data_cleanup_t,
                    pmix_list_item_t,
                    NULL, NULL);


static void dsicon(prte_ds_info_t *p)
{
    PMIX_PROC_CONSTRUCT(&p->source);
    PMIX_INFO_CONSTRUCT(&p->info);
}
static void dsides(prte_ds_info_t *p)
{
    PMIX_INFO_DESTRUCT(&p->info);
}
PMIX_CLASS_INSTANCE(prte_ds_info_t,
                    pmix_list_item_t,
                    dsicon, dsides);

