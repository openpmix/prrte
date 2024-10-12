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
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <string.h>

#include "src/mca/base/pmix_mca_base_component_repository.h"
#include "src/mca/mca.h"
#include "src/util/pmix_output.h"

#include "src/mca/state/state.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/rml/rml_contact.h"
#include "src/rml/oob/oob.h"

prte_rml_base_t prte_rml_base = {
    .rml_output = -1,
    .routed_output = -1,
    .posted_recvs = PMIX_LIST_STATIC_INIT,
    .unmatched_msgs = PMIX_LIST_STATIC_INIT,
    .max_retries = 0,
    .lifeline = PMIX_RANK_INVALID,
    .children = PMIX_LIST_STATIC_INIT,
    .radix = 64,
    .static_ports = false
};

static int verbosity = 0;

void prte_rml_register(void)
{
    int ret;

    prte_rml_base.max_retries = 3;
    pmix_mca_base_var_register("prte", "rml", "base", "max_retries",
                               "Max #times to retry sending a message",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_rml_base.max_retries);

    verbosity = 0;
    pmix_mca_base_var_register("prte", "rml", "base", "verbose",
                               "Debug verbosity of the RML subsystem",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &verbosity);
    if (0 < verbosity) {
        prte_rml_base.rml_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_rml_base.rml_output, verbosity);
    }

    verbosity = 0;
    pmix_mca_base_var_register("prte", "routed", "base", "verbose",
                               "Debug verbosity of the Routed subsystem",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &verbosity);
    if (0 < verbosity) {
        prte_rml_base.routed_output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_rml_base.routed_output, verbosity);
    }

    ret = pmix_mca_base_var_register("prte", "rml", "base", "radix",
                                     "Radix to be used for routing tree",
                                     PMIX_MCA_BASE_VAR_TYPE_INT,
                                     &prte_rml_base.radix);
    pmix_mca_base_var_register_synonym(ret, "prte", "routed", "radix", NULL,
                                       PMIX_MCA_BASE_VAR_SYN_FLAG_DEPRECATED);

    prte_oob_register();

    verbosity = 0;
    pmix_mca_base_var_register("prte", "oob", "base", "verbose",
                               "Debug verbosity of the out-of-band subsystem",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &verbosity);
    if (0 < verbosity) {
        prte_oob_base.output = pmix_output_open(NULL);
        pmix_output_set_verbosity(prte_oob_base.output, verbosity);
    }
}

void prte_rml_close(void)
{
    prte_oob_close();
    PMIX_LIST_DESTRUCT(&prte_rml_base.posted_recvs);
    PMIX_LIST_DESTRUCT(&prte_rml_base.unmatched_msgs);
    PMIX_LIST_DESTRUCT(&prte_rml_base.children);
    if (0 <= prte_rml_base.rml_output) {
        pmix_output_close(prte_rml_base.rml_output);
    }
}

int prte_rml_open(void)
{
    char *uri = NULL;
    pmix_value_t val;
    int ret;

    /* construct object for holding the active plugin modules */
    PMIX_CONSTRUCT(&prte_rml_base.posted_recvs, pmix_list_t);
    PMIX_CONSTRUCT(&prte_rml_base.unmatched_msgs, pmix_list_t);
    PMIX_CONSTRUCT(&prte_rml_base.children, pmix_list_t);

    /* compute the routing tree - only thing we need to know is the
     * number of daemons in the DVM */
    prte_rml_compute_routing_tree();

    prte_rml_base.lifeline = PRTE_PROC_MY_PARENT->rank;

    prte_oob_open();

    /* store our URI for later */
    prte_oob_base_get_addr(&uri);
    PMIX_VALUE_LOAD(&val, uri, PMIX_STRING);
    ret = PMIx_Store_internal(PRTE_PROC_MY_NAME, PMIX_PROC_URI, &val);
    if (PMIX_SUCCESS != ret) {
        PRTE_ERROR_LOG(PRTE_ERROR);
        PMIX_VALUE_DESTRUCT(&val);
        return PRTE_ERROR;
    }
    PMIX_VALUE_DESTRUCT(&val);
    // add it to our local info
    prte_process_info.my_uri = strdup(uri);

    if (PRTE_PROC_IS_MASTER) {
        prte_process_info.my_hnp_uri = uri;
    } else {
        free(uri);
        if (NULL == prte_process_info.my_hnp_uri) {
            // this is an error
            PRTE_ERROR_LOG(PRTE_ERROR);
            return PRTE_ERROR;
        }
        /* extract the HNP's name so we can update the routing table */
        ret = prte_rml_parse_uris(prte_process_info.my_hnp_uri,
                                  PRTE_PROC_MY_HNP,
                                  NULL);
        if (PRTE_SUCCESS != ret) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        /* Set the contact info in the RML - this won't actually establish
         * the connection, but just tells the RML how to reach the HNP
         * if/when we attempt to send to it
         */
        PMIX_VALUE_LOAD(&val, prte_process_info.my_hnp_uri, PMIX_STRING);
        ret = PMIx_Store_internal(PRTE_PROC_MY_HNP, PMIX_PROC_URI, &val);
        if (PMIX_SUCCESS != ret) {
            PRTE_ERROR_LOG(ret);
            PMIX_VALUE_DESTRUCT(&val);
            return ret;
        }
        PMIX_VALUE_DESTRUCT(&val);
    }

    return PRTE_SUCCESS;
}

void prte_rml_send_callback(int status, pmix_proc_t *peer,
                            pmix_data_buffer_t *buffer,
                            prte_rml_tag_t tag, void *cbdata)

{
    PRTE_HIDE_UNUSED_PARAMS(buffer, cbdata);

    if (PRTE_SUCCESS != status) {
        pmix_output_verbose(2, prte_rml_base.rml_output,
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

/***   RML CLASS INSTANCES   ***/
static void send_cons(prte_rml_send_t *ptr)
{
    ptr->retries = 0;
    ptr->cbdata = NULL;
    ptr->dbuf = NULL;
    ptr->seq_num = 0xFFFFFFFF;
}
static void send_des(prte_rml_send_t *ptr)
{
    if (ptr->dbuf != NULL)
        PMIX_DATA_BUFFER_RELEASE(ptr->dbuf);
}
PMIX_CLASS_INSTANCE(prte_rml_send_t, pmix_list_item_t, send_cons, send_des);

static void send_req_cons(prte_rml_send_request_t *ptr)
{
    PMIX_CONSTRUCT(&ptr->send, prte_rml_send_t);
}
static void send_req_des(prte_rml_send_request_t *ptr)
{
    PMIX_DESTRUCT(&ptr->send);
}
PMIX_CLASS_INSTANCE(prte_rml_send_request_t, pmix_object_t, send_req_cons, send_req_des);

static void recv_cons(prte_rml_recv_t *ptr)
{
    ptr->dbuf = NULL;
}
static void recv_des(prte_rml_recv_t *ptr)
{
    if (ptr->dbuf != NULL)
        PMIX_DATA_BUFFER_RELEASE(ptr->dbuf);
}
PMIX_CLASS_INSTANCE(prte_rml_recv_t, pmix_list_item_t, recv_cons, recv_des);

static void rcv_cons(prte_rml_recv_cb_t *ptr)
{
    PMIX_DATA_BUFFER_CONSTRUCT(&ptr->data);
    ptr->active = false;
}
static void rcv_des(prte_rml_recv_cb_t *ptr)
{
    PMIX_DATA_BUFFER_DESTRUCT(&ptr->data);
}
PMIX_CLASS_INSTANCE(prte_rml_recv_cb_t, pmix_object_t, rcv_cons, rcv_des);

static void prcv_cons(prte_rml_posted_recv_t *ptr)
{
    ptr->cbdata = NULL;
}
PMIX_CLASS_INSTANCE(prte_rml_posted_recv_t, pmix_list_item_t, prcv_cons, NULL);

static void prq_cons(prte_rml_recv_request_t *ptr)
{
    ptr->cancel = false;
    ptr->post = PMIX_NEW(prte_rml_posted_recv_t);
}
static void prq_des(prte_rml_recv_request_t *ptr)
{
    if (NULL != ptr->post) {
        PMIX_RELEASE(ptr->post);
    }
}
PMIX_CLASS_INSTANCE(prte_rml_recv_request_t, pmix_object_t, prq_cons, prq_des);

static void rtcon(prte_routed_tree_t *rt)
{
    rt->rank = PMIX_RANK_INVALID;
    PMIX_CONSTRUCT(&rt->relatives, pmix_bitmap_t);
}
static void rtdes(prte_routed_tree_t *rt)
{
    PMIX_DESTRUCT(&rt->relatives);
}
PMIX_CLASS_INSTANCE(prte_routed_tree_t,
                    pmix_list_item_t,
                    rtcon, rtdes);
