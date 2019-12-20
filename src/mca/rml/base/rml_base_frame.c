/*
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2013      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <string.h>

#include "src/dss/dss.h"
#include "src/mca/mca.h"
#include "src/mca/base/prrte_mca_base_component_repository.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/runtime/prrte_wait.h"
#include "src/util/name_fns.h"
#include "src/threads/threads.h"

#include "src/mca/rml/base/base.h"

/* The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct. */
#include "src/mca/rml/base/static-components.h"


/* Initialising stub fns in the global var used by other modules */
prrte_rml_base_module_t prrte_rml = {0};

prrte_rml_base_t prrte_rml_base = {{{0}}};

static int prrte_rml_base_register(prrte_mca_base_register_flag_t flags)
{
    prrte_rml_base.max_retries = 3;
    prrte_mca_base_var_register("prrte", "rml", "base", "max_retries",
                                 "Max #times to retry sending a message",
                                 PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                 PRRTE_INFO_LVL_9,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                 &prrte_rml_base.max_retries);

    return PRRTE_SUCCESS;
}

static int prrte_rml_base_close(void)
{
    PRRTE_LIST_DESTRUCT(&prrte_rml_base.posted_recvs);
    return prrte_mca_base_framework_components_close(&prrte_rml_base_framework, NULL);
}

static int prrte_rml_base_open(prrte_mca_base_open_flag_t flags)
{
    /* Initialize globals */
    /* construct object for holding the active plugin modules */
    PRRTE_CONSTRUCT(&prrte_rml_base.posted_recvs, prrte_list_t);
    PRRTE_CONSTRUCT(&prrte_rml_base.unmatched_msgs, prrte_list_t);

    /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_rml_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, rml, "PRRTE Run-Time Messaging Layer",
                                 prrte_rml_base_register, prrte_rml_base_open, prrte_rml_base_close,
                                 prrte_rml_base_static_components, 0);

/**
 * Function for ordering the component(plugin) by priority
 */
int prrte_rml_base_select(void)
{
    prrte_rml_component_t *best_component = NULL;
    prrte_rml_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PRRTE_SUCCESS != prrte_mca_base_select("rml", prrte_rml_base_framework.framework_output,
                                                &prrte_rml_base_framework.framework_components,
                                                (prrte_mca_base_module_t **) &best_module,
                                                (prrte_mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected */
        /* If we didn't find one to select, that is an error */
        return PRRTE_ERROR;
    }

    /* Save the winner */
    prrte_rml = *best_module;

    return PRRTE_SUCCESS;
}

void prrte_rml_send_callback(int status, prrte_process_name_t *peer,
                            prrte_buffer_t* buffer, prrte_rml_tag_t tag,
                            void* cbdata)

{
    PRRTE_RELEASE(buffer);
    if (PRRTE_SUCCESS != status) {
        prrte_output_verbose(2, prrte_rml_base_framework.framework_output,
                            "%s UNABLE TO SEND MESSAGE TO %s TAG %d: %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(peer), tag,
                            PRRTE_ERROR_NAME(status));
        if (PRRTE_ERR_NO_PATH_TO_TARGET == status) {
            PRRTE_ACTIVATE_PROC_STATE(peer, PRRTE_PROC_STATE_NO_PATH_TO_TARGET);
        } else if (PRRTE_ERR_ADDRESSEE_UNKNOWN == status) {
            PRRTE_ACTIVATE_PROC_STATE(peer, PRRTE_PROC_STATE_PEER_UNKNOWN);
        } else {
            PRRTE_ACTIVATE_PROC_STATE(peer, PRRTE_PROC_STATE_UNABLE_TO_SEND_MSG);
        }
    }
}

void prrte_rml_recv_callback(int status, prrte_process_name_t* sender,
                            prrte_buffer_t *buffer,
                            prrte_rml_tag_t tag, void *cbdata)
{
    prrte_rml_recv_cb_t *blob = (prrte_rml_recv_cb_t*)cbdata;

    PRRTE_ACQUIRE_OBJECT(blob);
    /* transfer the sender */
    blob->name.jobid = sender->jobid;
    blob->name.vpid = sender->vpid;
    /* just copy the payload to the buf */
    prrte_dss.copy_payload(&blob->data, buffer);
    /* flag as complete */
    PRRTE_POST_OBJECT(blob);
    blob->active = false;
}


/***   RML CLASS INSTANCES   ***/
static void xfer_cons(prrte_self_send_xfer_t *xfer)
{
    xfer->iov = NULL;
    xfer->cbfunc.iov = NULL;
    xfer->buffer = NULL;
    xfer->cbfunc.buffer = NULL;
    xfer->cbdata = NULL;
}
PRRTE_CLASS_INSTANCE(prrte_self_send_xfer_t,
                   prrte_object_t,
                   xfer_cons, NULL);

static void send_cons(prrte_rml_send_t *ptr)
{
    ptr->retries = 0;
    ptr->cbdata = NULL;
    ptr->iov = NULL;
    ptr->buffer = NULL;
    ptr->data = NULL;
    ptr->seq_num = 0xFFFFFFFF;
}
PRRTE_CLASS_INSTANCE(prrte_rml_send_t,
                   prrte_list_item_t,
                   send_cons, NULL);


static void send_req_cons(prrte_rml_send_request_t *ptr)
{
    PRRTE_CONSTRUCT(&ptr->send, prrte_rml_send_t);
}
static void send_req_des(prrte_rml_send_request_t *ptr)
{
    PRRTE_DESTRUCT(&ptr->send);
}
PRRTE_CLASS_INSTANCE(prrte_rml_send_request_t,
                   prrte_object_t,
                   send_req_cons, send_req_des);

static void recv_cons(prrte_rml_recv_t *ptr)
{
    ptr->iov.iov_base = NULL;
    ptr->iov.iov_len = 0;
}
static void recv_des(prrte_rml_recv_t *ptr)
{
    if (NULL != ptr->iov.iov_base) {
        free(ptr->iov.iov_base);
    }
}
PRRTE_CLASS_INSTANCE(prrte_rml_recv_t,
                   prrte_list_item_t,
                   recv_cons, recv_des);

static void rcv_cons(prrte_rml_recv_cb_t *ptr)
{
    PRRTE_CONSTRUCT(&ptr->data, prrte_buffer_t);
    ptr->active = false;
}
static void rcv_des(prrte_rml_recv_cb_t *ptr)
{
    PRRTE_DESTRUCT(&ptr->data);
}
PRRTE_CLASS_INSTANCE(prrte_rml_recv_cb_t, prrte_object_t,
                   rcv_cons, rcv_des);

static void prcv_cons(prrte_rml_posted_recv_t *ptr)
{
    ptr->cbdata = NULL;
}
PRRTE_CLASS_INSTANCE(prrte_rml_posted_recv_t,
                   prrte_list_item_t,
                   prcv_cons, NULL);

static void prq_cons(prrte_rml_recv_request_t *ptr)
{
    ptr->cancel = false;
    ptr->post = PRRTE_NEW(prrte_rml_posted_recv_t);
}
static void prq_des(prrte_rml_recv_request_t *ptr)
{
    if (NULL != ptr->post) {
        PRRTE_RELEASE(ptr->post);
    }
}
PRRTE_CLASS_INSTANCE(prrte_rml_recv_request_t,
                   prrte_object_t,
                   prq_cons, prq_des);
