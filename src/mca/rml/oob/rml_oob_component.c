/* -*- Mode: C; c-basic-offset:4 ; -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include "src/mca/base/base.h"
#include "src/util/output.h"
#include "src/util/argv.h"
#include "src/mca/backtrace/backtrace.h"
#include "src/event/event-internal.h"

#include "src/mca/rml/base/base.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/routed.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/oob/oob.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/routed/routed.h"
#include "rml_oob.h"

static int rml_oob_open(void);
static int rml_oob_close(void);
static int component_query(prrte_mca_base_module_t **module, int *priority);

/**
 * component definition
 */
prrte_rml_component_t prrte_rml_oob_component = {
      /* First, the prrte_mca_base_component_t struct containing meta
         information about the component itself */

    .base = {
        PRRTE_RML_BASE_VERSION_3_0_0,

        .mca_component_name = "oob",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),
        .mca_open_component = rml_oob_open,
        .mca_close_component = rml_oob_close,
        .mca_query_component = component_query,

    },
    .data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
    .priority = 5
};

/* Local variables */
static void recv_nb(prrte_process_name_t* peer,
                    prrte_rml_tag_t tag,
                    bool persistent,
                    prrte_rml_callback_fn_t cbfunc,
                    void* cbdata)
{
    prrte_rml_recv_request_t *req;

    prrte_output_verbose(10, prrte_rml_base_framework.framework_output,
                         "%s rml_recv_nb for peer %s tag %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(peer), tag);

    /* push the request into the event base so we can add
     * the receive to our list of posted recvs */
    req = PRRTE_NEW(prrte_rml_recv_request_t);
    req->post->buffer_data = false;
    req->post->peer.jobid = peer->jobid;
    req->post->peer.vpid = peer->vpid;
    req->post->tag = tag;
    req->post->persistent = persistent;
    req->post->cbfunc.iov = cbfunc;
    req->post->cbdata = cbdata;
    PRRTE_THREADSHIFT(req, prrte_event_base, prrte_rml_base_post_recv, PRRTE_MSG_PRI);
}
static void recv_buffer_nb(prrte_process_name_t* peer,
                           prrte_rml_tag_t tag,
                           bool persistent,
                           prrte_rml_buffer_callback_fn_t cbfunc,
                           void* cbdata)
{
    prrte_rml_recv_request_t *req;

    prrte_output_verbose(10, prrte_rml_base_framework.framework_output,
                         "%s rml_recv_buffer_nb for peer %s tag %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(peer), tag);

    /* push the request into the event base so we can add
     * the receive to our list of posted recvs */
    req = PRRTE_NEW(prrte_rml_recv_request_t);
    req->post->buffer_data = true;
    req->post->peer.jobid = peer->jobid;
    req->post->peer.vpid = peer->vpid;
    req->post->tag = tag;
    req->post->persistent = persistent;
    req->post->cbfunc.buffer = cbfunc;
    req->post->cbdata = cbdata;
    PRRTE_THREADSHIFT(req, prrte_event_base, prrte_rml_base_post_recv, PRRTE_MSG_PRI);
}
static void recv_cancel(prrte_process_name_t* peer, prrte_rml_tag_t tag)
{
    prrte_rml_recv_request_t *req;

    prrte_output_verbose(10, prrte_rml_base_framework.framework_output,
                         "%s rml_recv_cancel for peer %s tag %d",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(peer), tag);

    PRRTE_ACQUIRE_OBJECT(prrte_event_base_active);
    if (!prrte_event_base_active) {
        /* no event will be processed any more, so simply return. */
        return;
    }

    /* push the request into the event base so we can remove
     * the receive from our list of posted recvs */
    req = PRRTE_NEW(prrte_rml_recv_request_t);
    req->cancel = true;
    req->post->peer.jobid = peer->jobid;
    req->post->peer.vpid = peer->vpid;
    req->post->tag = tag;
    PRRTE_THREADSHIFT(req, prrte_event_base, prrte_rml_base_post_recv, PRRTE_MSG_PRI);
}
static int oob_ping(const char* uri, const struct timeval* tv)
{
    return PRRTE_ERR_UNREACH;
}

static prrte_rml_base_module_t base_module = {
    .component = (struct prrte_rml_component_t*)&prrte_rml_oob_component,
    .ping = oob_ping,
    .send_nb = prrte_rml_oob_send_nb,
    .send_buffer_nb = prrte_rml_oob_send_buffer_nb,
    .recv_nb = recv_nb,
    .recv_buffer_nb = recv_buffer_nb,
    .recv_cancel = recv_cancel,
    .purge = NULL
};

static int rml_oob_open(void)
{
    return PRRTE_SUCCESS;
}


static int rml_oob_close(void)
{
    return PRRTE_SUCCESS;
}

static int component_query(prrte_mca_base_module_t **module, int *priority)
{
    *priority = 50;
    *module = (prrte_mca_base_module_t *) &base_module;
    return PRRTE_SUCCESS;
}
