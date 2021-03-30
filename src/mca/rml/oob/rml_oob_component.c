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
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif

#include "src/event/event-internal.h"
#include "src/mca/base/base.h"
#include "src/mca/prtebacktrace/prtebacktrace.h"
#include "src/util/argv.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/rml/base/base.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/routed.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "rml_oob.h"
#include "src/mca/oob/base/base.h"
#include "src/mca/oob/oob.h"
#include "src/mca/routed/routed.h"

static int rml_oob_open(void);
static int rml_oob_close(void);
static int component_query(prte_mca_base_module_t **module, int *priority);

/**
 * component definition
 */
prte_rml_component_t prte_rml_oob_component = {
      /* First, the prte_mca_base_component_t struct containing meta
         information about the component itself */

    .base = {
        PRTE_RML_BASE_VERSION_3_0_0,

        .mca_component_name = "oob",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                    PRTE_RELEASE_VERSION),
        .mca_open_component = rml_oob_open,
        .mca_close_component = rml_oob_close,
        .mca_query_component = component_query,

    },
    .data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
    .priority = 5
};

/* Local variables */
static void recv_buffer_nb(pmix_proc_t *peer, prte_rml_tag_t tag, bool persistent,
                           prte_rml_buffer_callback_fn_t cbfunc, void *cbdata)
{
    prte_rml_recv_request_t *req;

    prte_output_verbose(10, prte_rml_base_framework.framework_output,
                        "%s rml_recv_buffer_nb for peer %s tag %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(peer), tag);

    /* push the request into the event base so we can add
     * the receive to our list of posted recvs */
    req = PRTE_NEW(prte_rml_recv_request_t);
    PMIX_XFER_PROCID(&req->post->peer, peer);
    req->post->tag = tag;
    req->post->persistent = persistent;
    req->post->cbfunc = cbfunc;
    req->post->cbdata = cbdata;
    PRTE_THREADSHIFT(req, prte_event_base, prte_rml_base_post_recv, PRTE_MSG_PRI);
}
static void recv_cancel(pmix_proc_t *peer, prte_rml_tag_t tag)
{
    prte_rml_recv_request_t *req;

    prte_output_verbose(10, prte_rml_base_framework.framework_output,
                        "%s rml_recv_cancel for peer %s tag %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(peer), tag);

    PRTE_ACQUIRE_OBJECT(prte_event_base_active);
    if (!prte_event_base_active) {
        /* no event will be processed any more, so simply return. */
        return;
    }

    /* push the request into the event base so we can remove
     * the receive from our list of posted recvs */
    req = PRTE_NEW(prte_rml_recv_request_t);
    req->cancel = true;
    PMIX_XFER_PROCID(&req->post->peer, peer);
    req->post->tag = tag;
    PRTE_THREADSHIFT(req, prte_event_base, prte_rml_base_post_recv, PRTE_MSG_PRI);
}
static int oob_ping(const char *uri, const struct timeval *tv)
{
    return PRTE_ERR_UNREACH;
}

static prte_rml_base_module_t base_module = {.component = (struct prte_rml_component_t
                                                               *) &prte_rml_oob_component,
                                             .ping = oob_ping,
                                             .send_buffer_nb = prte_rml_oob_send_buffer_nb,
                                             .recv_buffer_nb = recv_buffer_nb,
                                             .recv_cancel = recv_cancel,
                                             .purge = NULL};

static int rml_oob_open(void)
{
    return PRTE_SUCCESS;
}

static int rml_oob_close(void)
{
    return PRTE_SUCCESS;
}

static int component_query(prte_mca_base_module_t **module, int *priority)
{
    *priority = 50;
    *module = (prte_mca_base_module_t *) &base_module;
    return PRTE_SUCCESS;
}
