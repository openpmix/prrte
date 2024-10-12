/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2006-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      NVIDIA Corporation.  All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * In windows, many of the socket functions return an EWOULDBLOCK
 * instead of things like EAGAIN, EINPROGRESS, etc. It has been
 * verified that this will not conflict with other error codes that
 * are returned by these functions under UNIX/Linux environments
 */

#include "prte_config.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <fcntl.h>
#ifdef HAVE_NET_IF_H
#    include <net/if.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif
#include <ctype.h>
#include <sys/socket.h>

#ifndef MIN
#    define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#include "src/class/pmix_list.h"
#include "src/event/event-internal.h"
#include "src/include/prte_socket_errno.h"
#include "src/runtime/prte_progress_threads.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_if.h"
#include "src/util/error.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/rml/rml.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"
#include "src/util/attr.h"
#include "src/util/name_fns.h"
#include "src/util/pmix_parse_options.h"
#include "src/util/pmix_show_help.h"

#include "src/rml/oob/oob_tcp_peer.h"
#include "src/rml/oob/oob_tcp.h"
#include "src/rml/oob/oob_tcp_common.h"
#include "src/rml/oob/oob_tcp_connection.h"
#include "src/rml/oob/oob_tcp_listener.h"
#include "src/rml/oob/oob_tcp_peer.h"

void prte_mca_oob_tcp_component_lost_connection(int fd, short args, void *cbdata)
{
    prte_oob_tcp_peer_op_t *pop = (prte_oob_tcp_peer_op_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(pop);

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s tcp:lost connection called for peer %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&pop->peer));

    if (!prte_finalizing) {
        /* activate the proc state */
        if (PRTE_SUCCESS != prte_rml_route_lost(pop->peer.rank)) {
            PRTE_ACTIVATE_PROC_STATE(&pop->peer, PRTE_PROC_STATE_LIFELINE_LOST);
        } else {
            PRTE_ACTIVATE_PROC_STATE(&pop->peer, PRTE_PROC_STATE_COMM_FAILED);
        }
    }
    PMIX_RELEASE(pop);
}

void prte_mca_oob_tcp_component_no_route(int fd, short args, void *cbdata)
{
    prte_oob_tcp_msg_error_t *mop = (prte_oob_tcp_msg_error_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(mop);

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s tcp:no route called for peer %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&mop->hop));

    if (prte_prteds_term_ordered || prte_finalizing || prte_abnormal_term_ordered) {
        /* just ignore the problem */
        PMIX_RELEASE(mop);
        return;
    }

    /* report the error */
    PRTE_ACTIVATE_PROC_STATE(&mop->hop, PRTE_PROC_STATE_UNABLE_TO_SEND_MSG);

    PMIX_RELEASE(mop);
}

void prte_mca_oob_tcp_component_hop_unknown(int fd, short args, void *cbdata)
{
    prte_oob_tcp_msg_error_t *mop = (prte_oob_tcp_msg_error_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(mop);

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s tcp:unknown hop called for peer %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&mop->hop));

    if (prte_prteds_term_ordered || prte_finalizing || prte_abnormal_term_ordered) {
        /* just ignore the problem */
        PMIX_RELEASE(mop);
        return;
    }

    /* post the error */
    PRTE_ACTIVATE_PROC_STATE(&mop->hop, PRTE_PROC_STATE_UNABLE_TO_SEND_MSG);

    PMIX_RELEASE(mop);
}

void prte_mca_oob_tcp_component_failed_to_connect(int fd, short args, void *cbdata)
{
    prte_oob_tcp_peer_op_t *pop = (prte_oob_tcp_peer_op_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(pop);

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s tcp:failed_to_connect called for peer %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&pop->peer));

    /* if we are terminating, then don't attempt to reconnect */
    if (prte_prteds_term_ordered || prte_finalizing || prte_abnormal_term_ordered) {
        PMIX_RELEASE(pop);
        return;
    }

    /* activate the proc state */
    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s tcp:failed_to_connect unable to reach peer %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&pop->peer));

    PRTE_ACTIVATE_PROC_STATE(&pop->peer, PRTE_PROC_STATE_FAILED_TO_CONNECT);
    PMIX_RELEASE(pop);
}


/* OOB TCP Class instances */

static void peer_cons(prte_oob_tcp_peer_t *peer)
{
    peer->auth_method = NULL;
    peer->sd = -1;
    PMIX_CONSTRUCT(&peer->addrs, pmix_list_t);
    peer->active_addr = NULL;
    peer->state = MCA_OOB_TCP_UNCONNECTED;
    peer->num_retries = 0;
    PMIX_CONSTRUCT(&peer->send_queue, pmix_list_t);
    peer->send_msg = NULL;
    peer->recv_msg = NULL;
    peer->send_ev_active = false;
    peer->recv_ev_active = false;
    peer->timer_ev_active = false;
}
static void peer_des(prte_oob_tcp_peer_t *peer)
{
    if (NULL != peer->auth_method) {
        free(peer->auth_method);
    }
    if (peer->send_ev_active) {
        prte_event_del(&peer->send_event);
    }
    if (peer->recv_ev_active) {
        prte_event_del(&peer->recv_event);
    }
    if (peer->timer_ev_active) {
        prte_event_del(&peer->timer_event);
    }
    if (0 <= peer->sd) {
        pmix_output_verbose(2, prte_oob_base.output,
                            "%s CLOSING SOCKET %d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), peer->sd);
        CLOSE_THE_SOCKET(peer->sd);
    }
    PMIX_LIST_DESTRUCT(&peer->addrs);
    PMIX_LIST_DESTRUCT(&peer->send_queue);
}
PMIX_CLASS_INSTANCE(prte_oob_tcp_peer_t, pmix_list_item_t, peer_cons, peer_des);

static void padd_cons(prte_oob_tcp_addr_t *ptr)
{
    memset(&ptr->addr, 0, sizeof(ptr->addr));
    ptr->retries = 0;
    ptr->state = MCA_OOB_TCP_UNCONNECTED;
}
PMIX_CLASS_INSTANCE(prte_oob_tcp_addr_t, pmix_list_item_t, padd_cons, NULL);

static void pop_cons(prte_oob_tcp_peer_op_t *pop)
{
    pop->net = NULL;
    pop->port = NULL;
}
static void pop_des(prte_oob_tcp_peer_op_t *pop)
{
    if (NULL != pop->net) {
        free(pop->net);
    }
    if (NULL != pop->port) {
        free(pop->port);
    }
}
PMIX_CLASS_INSTANCE(prte_oob_tcp_peer_op_t, pmix_object_t, pop_cons, pop_des);

PMIX_CLASS_INSTANCE(prte_oob_tcp_msg_op_t, pmix_object_t, NULL, NULL);

PMIX_CLASS_INSTANCE(prte_oob_tcp_conn_op_t, pmix_object_t, NULL, NULL);

static void nicaddr_cons(prte_oob_tcp_nicaddr_t *ptr)
{
    ptr->af_family = PF_UNSPEC;
    memset(&ptr->addr, 0, sizeof(ptr->addr));
}
PMIX_CLASS_INSTANCE(prte_oob_tcp_nicaddr_t, pmix_list_item_t, nicaddr_cons, NULL);
