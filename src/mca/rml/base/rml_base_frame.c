/*
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <string.h>

#include "src/mca/base/prte_mca_base_component_repository.h"
#include "src/mca/mca.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/threads.h"
#include "src/util/name_fns.h"

#include "src/mca/rml/base/base.h"

/* The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prte_mca_base_component_t struct. */
#include "src/mca/rml/base/static-components.h"

/* Initialising stub fns in the global var used by other modules */
prte_rml_base_module_t prte_rml = {0};

prte_rml_base_t prte_rml_base = {{{0}}};

static int prte_rml_base_register(prte_mca_base_register_flag_t flags)
{
    prte_rml_base.max_retries = 3;
    prte_mca_base_var_register("prte", "rml", "base", "max_retries",
                               "Max #times to retry sending a message", PRTE_MCA_BASE_VAR_TYPE_INT,
                               NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                               PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_rml_base.max_retries);

    return PRTE_SUCCESS;
}

static int prte_rml_base_close(void)
{
    PRTE_LIST_DESTRUCT(&prte_rml_base.posted_recvs);
    return prte_mca_base_framework_components_close(&prte_rml_base_framework, NULL);
}

static int prte_rml_base_open(prte_mca_base_open_flag_t flags)
{
    /* Initialize globals */
    /* construct object for holding the active plugin modules */
    PRTE_CONSTRUCT(&prte_rml_base.posted_recvs, prte_list_t);
    PRTE_CONSTRUCT(&prte_rml_base.unmatched_msgs, prte_list_t);

    /* Open up all available components */
    return prte_mca_base_framework_components_open(&prte_rml_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, rml, "PRTE Run-Time Messaging Layer", prte_rml_base_register,
                                prte_rml_base_open, prte_rml_base_close,
                                prte_rml_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

/**
 * Function for ordering the component(plugin) by priority
 */
int prte_rml_base_select(void)
{
    prte_rml_component_t *best_component = NULL;
    prte_rml_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if (PRTE_SUCCESS
        != prte_mca_base_select("rml", prte_rml_base_framework.framework_output,
                                &prte_rml_base_framework.framework_components,
                                (prte_mca_base_module_t **) &best_module,
                                (prte_mca_base_component_t **) &best_component, NULL)) {
        /* This will only happen if no component was selected */
        /* If we didn't find one to select, that is an error */
        return PRTE_ERROR;
    }

    /* Save the winner */
    prte_rml = *best_module;

    return PRTE_SUCCESS;
}

void prte_rml_send_callback(int status, pmix_proc_t *peer, pmix_data_buffer_t *buffer,
                            prte_rml_tag_t tag, void *cbdata)

{
    if (PRTE_SUCCESS != status) {
        prte_output_verbose(2, prte_rml_base_framework.framework_output,
                            "%s UNABLE TO SEND MESSAGE TO %s TAG %d: %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(peer), tag,
                            PRTE_ERROR_NAME(status));
        if (PRTE_ERR_NO_PATH_TO_TARGET == status) {
            PRTE_ACTIVATE_PROC_STATE(peer, PRTE_PROC_STATE_NO_PATH_TO_TARGET);
        } else if (PRTE_ERR_ADDRESSEE_UNKNOWN == status) {
            PRTE_ACTIVATE_PROC_STATE(peer, PRTE_PROC_STATE_PEER_UNKNOWN);
        } else {
            PRTE_ACTIVATE_PROC_STATE(peer, PRTE_PROC_STATE_UNABLE_TO_SEND_MSG);
        }
    }
}

void prte_rml_recv_callback(int status, pmix_proc_t *sender, pmix_data_buffer_t *buffer,
                            prte_rml_tag_t tag, void *cbdata)
{
    prte_rml_recv_cb_t *blob = (prte_rml_recv_cb_t *) cbdata;
    pmix_status_t rc;

    PRTE_ACQUIRE_OBJECT(blob);
    /* transfer the sender */
    PMIX_LOAD_PROCID(&blob->name, sender->nspace, sender->rank);
    /* just copy the payload to the buf */
    rc = PMIx_Data_copy_payload(&blob->data, buffer);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
    }
    /* flag as complete */
    PRTE_POST_OBJECT(blob);
    blob->active = false;
}

/***   RML CLASS INSTANCES   ***/
static void xfer_cons(prte_self_send_xfer_t *xfer)
{
    PMIX_DATA_BUFFER_CONSTRUCT(&xfer->dbuf);
    xfer->cbfunc = NULL;
    xfer->cbdata = NULL;
}
static void xfer_des(prte_self_send_xfer_t *xfer)
{
    PMIX_DATA_BUFFER_DESTRUCT(&xfer->dbuf);
}
PRTE_CLASS_INSTANCE(prte_self_send_xfer_t, prte_object_t, xfer_cons, xfer_des);

static void send_cons(prte_rml_send_t *ptr)
{
    ptr->retries = 0;
    ptr->cbdata = NULL;
    PMIX_DATA_BUFFER_CONSTRUCT(&ptr->dbuf);
    ptr->seq_num = 0xFFFFFFFF;
}
static void send_des(prte_rml_send_t *ptr)
{
    PMIX_DATA_BUFFER_DESTRUCT(&ptr->dbuf);
}
PRTE_CLASS_INSTANCE(prte_rml_send_t, prte_list_item_t, send_cons, send_des);

static void send_req_cons(prte_rml_send_request_t *ptr)
{
    PRTE_CONSTRUCT(&ptr->send, prte_rml_send_t);
}
static void send_req_des(prte_rml_send_request_t *ptr)
{
    PRTE_DESTRUCT(&ptr->send);
}
PRTE_CLASS_INSTANCE(prte_rml_send_request_t, prte_object_t, send_req_cons, send_req_des);

static void recv_cons(prte_rml_recv_t *ptr)
{
    PMIX_DATA_BUFFER_CONSTRUCT(&ptr->dbuf);
}
static void recv_des(prte_rml_recv_t *ptr)
{
    PMIX_DATA_BUFFER_DESTRUCT(&ptr->dbuf);
}
PRTE_CLASS_INSTANCE(prte_rml_recv_t, prte_list_item_t, recv_cons, recv_des);

static void rcv_cons(prte_rml_recv_cb_t *ptr)
{
    PMIX_DATA_BUFFER_CONSTRUCT(&ptr->data);
    ptr->active = false;
}
static void rcv_des(prte_rml_recv_cb_t *ptr)
{
    PMIX_DATA_BUFFER_DESTRUCT(&ptr->data);
}
PRTE_CLASS_INSTANCE(prte_rml_recv_cb_t, prte_object_t, rcv_cons, rcv_des);

static void prcv_cons(prte_rml_posted_recv_t *ptr)
{
    ptr->cbdata = NULL;
}
PRTE_CLASS_INSTANCE(prte_rml_posted_recv_t, prte_list_item_t, prcv_cons, NULL);

static void prq_cons(prte_rml_recv_request_t *ptr)
{
    ptr->cancel = false;
    ptr->post = PRTE_NEW(prte_rml_posted_recv_t);
}
static void prq_des(prte_rml_recv_request_t *ptr)
{
    if (NULL != ptr->post) {
        PRTE_RELEASE(ptr->post);
    }
}
PRTE_CLASS_INSTANCE(prte_rml_recv_request_t, prte_object_t, prq_cons, prq_des);
