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
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2009-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
#include "types.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <fcntl.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#include <ctype.h>

#include "src/runtime/prrte_progress_threads.h"
#include "src/util/show_help.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/include/prrte_socket_errno.h"
#include "src/util/if.h"
#include "src/util/net.h"
#include "src/util/argv.h"
#include "src/class/prrte_hash_table.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/routed/routed.h"
#include "src/util/name_fns.h"
#include "src/util/parse_options.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/oob/tcp/oob_tcp.h"
#include "src/mca/oob/tcp/oob_tcp_component.h"
#include "src/mca/oob/tcp/oob_tcp_peer.h"
#include "src/mca/oob/tcp/oob_tcp_common.h"
#include "src/mca/oob/tcp/oob_tcp_connection.h"
#include "src/mca/oob/tcp/oob_tcp_sendrecv.h"

static void accept_connection(const int accepted_fd,
                              const struct sockaddr *addr);
static void ping(const prrte_process_name_t *proc);
static void send_nb(prrte_rml_send_t *msg);

prrte_oob_tcp_module_t prrte_oob_tcp_module = {
    .accept_connection = accept_connection,
    .ping = ping,
    .send_nb = send_nb
};

/*
 * Local utility functions
 */
static void recv_handler(int sd, short flags, void* user);

/* Called by prrte_oob_tcp_accept() and connection_handler() on
 * a socket that has been accepted.  This call finishes processing the
 * socket, including setting socket options and registering for the
 * OOB-level connection handshake.  Used in both the threaded and
 * event listen modes.
 */
static void accept_connection(const int accepted_fd,
                              const struct sockaddr *addr)
{
    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s accept_connection: %s:%d\n",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        prrte_net_get_hostname(addr),
                        prrte_net_get_port(addr));

   /* setup socket options */
    prrte_oob_tcp_set_socket_options(accepted_fd);

    /* use a one-time event to wait for receipt of peer's
     *  process ident message to complete this connection
     */
    PRRTE_ACTIVATE_TCP_ACCEPT_STATE(accepted_fd, addr, recv_handler);
}

/* API functions */
static void ping(const prrte_process_name_t *proc)
{
    prrte_oob_tcp_peer_t *peer;

    prrte_output_verbose(2, prrte_oob_base_framework.framework_output,
                        "%s:[%s:%d] processing ping to peer %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        __FILE__, __LINE__,
                        PRRTE_NAME_PRINT(proc));

    /* do we know this peer? */
    if (NULL == (peer = prrte_oob_tcp_peer_lookup(proc))) {
        /* push this back to the component so it can try
         * another module within this transport. If no
         * module can be found, the component can push back
         * to the framework so another component can try
         */
        prrte_output_verbose(2, prrte_oob_base_framework.framework_output,
                            "%s:[%s:%d] hop %s unknown",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            __FILE__, __LINE__,
                            PRRTE_NAME_PRINT(proc));
        PRRTE_ACTIVATE_TCP_MSG_ERROR(NULL, NULL, proc, prrte_oob_tcp_component_hop_unknown);
        return;
    }

    /* if we are already connected, there is nothing to do */
    if (MCA_OOB_TCP_CONNECTED == peer->state) {
        prrte_output_verbose(2, prrte_oob_base_framework.framework_output,
                            "%s:[%s:%d] already connected to peer %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            __FILE__, __LINE__,
                            PRRTE_NAME_PRINT(proc));
        return;
    }

    /* if we are already connecting, there is nothing to do */
    if (MCA_OOB_TCP_CONNECTING == peer->state ||
        MCA_OOB_TCP_CONNECT_ACK == peer->state) {
        prrte_output_verbose(2, prrte_oob_base_framework.framework_output,
                            "%s:[%s:%d] already connecting to peer %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            __FILE__, __LINE__,
                            PRRTE_NAME_PRINT(proc));
        return;
    }

    /* attempt the connection */
    peer->state = MCA_OOB_TCP_CONNECTING;
    PRRTE_ACTIVATE_TCP_CONN_STATE(peer, prrte_oob_tcp_peer_try_connect);
}

static void send_nb(prrte_rml_send_t *msg)
{
    prrte_oob_tcp_peer_t *peer;
    prrte_process_name_t hop;


    /* do we have a route to this peer (could be direct)? */
    hop = prrte_routed.get_route(&msg->dst);
    /* do we know this hop? */
    if (NULL == (peer = prrte_oob_tcp_peer_lookup(&hop))) {
        /* push this back to the component so it can try
         * another module within this transport. If no
         * module can be found, the component can push back
         * to the framework so another component can try
         */
        prrte_output_verbose(2, prrte_oob_base_framework.framework_output,
                            "%s:[%s:%d] processing send to peer %s:%d seq_num = %d hop %s unknown",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            __FILE__, __LINE__,
                            PRRTE_NAME_PRINT(&msg->dst), msg->tag, msg->seq_num,
                            PRRTE_NAME_PRINT(&hop));
        PRRTE_ACTIVATE_TCP_NO_ROUTE(msg, &hop, prrte_oob_tcp_component_no_route);
        return;
    }

    prrte_output_verbose(2, prrte_oob_base_framework.framework_output,
                        "%s:[%s:%d] processing send to peer %s:%d seq_num = %d via %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        __FILE__, __LINE__,
                        PRRTE_NAME_PRINT(&msg->dst), msg->tag, msg->seq_num,
                        PRRTE_NAME_PRINT(&peer->name));

    /* add the msg to the hop's send queue */
    if (MCA_OOB_TCP_CONNECTED == peer->state) {
        prrte_output_verbose(2, prrte_oob_base_framework.framework_output,
                            "%s tcp:send_nb: already connected to %s - queueing for send",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&peer->name));
        MCA_OOB_TCP_QUEUE_SEND(msg, peer);
        return;
    }

    /* add the message to the queue for sending after the
     * connection is formed
     */
    MCA_OOB_TCP_QUEUE_PENDING(msg, peer);

    if (MCA_OOB_TCP_CONNECTING != peer->state &&
        MCA_OOB_TCP_CONNECT_ACK != peer->state) {
        /* we have to initiate the connection - again, we do not
         * want to block while the connection is created.
         * So throw us into an event that will create
         * the connection via a mini-state-machine :-)
         */
        prrte_output_verbose(2, prrte_oob_base_framework.framework_output,
                            "%s tcp:send_nb: initiating connection to %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&peer->name));
        peer->state = MCA_OOB_TCP_CONNECTING;
        PRRTE_ACTIVATE_TCP_CONN_STATE(peer, prrte_oob_tcp_peer_try_connect);
    }
}

/*
 * Event callback when there is data available on the registered
 * socket to recv.  This is called for the listen sockets to accept an
 * incoming connection, on new sockets trying to complete the software
 * connection process, and for probes.  Data on an established
 * connection is handled elsewhere.
 */
static void recv_handler(int sd, short flg, void *cbdata)
{
    prrte_oob_tcp_conn_op_t *op = (prrte_oob_tcp_conn_op_t*)cbdata;
    int flags;
    prrte_oob_tcp_hdr_t hdr;
    prrte_oob_tcp_peer_t *peer;

    PRRTE_ACQUIRE_OBJECT(op);

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s:tcp:recv:handler called",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* get the handshake */
    if (PRRTE_SUCCESS != prrte_oob_tcp_peer_recv_connect_ack(NULL, sd, &hdr)) {
        goto cleanup;
    }

    /* finish processing ident */
    if (MCA_OOB_TCP_IDENT == hdr.type) {
        if (NULL == (peer = prrte_oob_tcp_peer_lookup(&hdr.origin))) {
            /* should never happen */
            prrte_oob_tcp_peer_close(peer);
            goto cleanup;
        }
        /* set socket up to be non-blocking */
        if ((flags = fcntl(sd, F_GETFL, 0)) < 0) {
            prrte_output(0, "%s prrte_oob_tcp_recv_connect: fcntl(F_GETFL) failed: %s (%d)",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), strerror(prrte_socket_errno), prrte_socket_errno);
        } else {
            flags |= O_NONBLOCK;
            if (fcntl(sd, F_SETFL, flags) < 0) {
                prrte_output(0, "%s prrte_oob_tcp_recv_connect: fcntl(F_SETFL) failed: %s (%d)",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), strerror(prrte_socket_errno), prrte_socket_errno);
            }
        }
        /* is the peer instance willing to accept this connection */
        peer->sd = sd;
        if (prrte_oob_tcp_peer_accept(peer) == false) {
            if (OOB_TCP_DEBUG_CONNECT <= prrte_output_get_verbosity(prrte_oob_base_framework.framework_output)) {
                prrte_output(0, "%s-%s prrte_oob_tcp_recv_connect: "
                            "rejected connection from %s connection state %d",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&(peer->name)),
                            PRRTE_NAME_PRINT(&(hdr.origin)),
                            peer->state);
            }
            CLOSE_THE_SOCKET(sd);
        }
    }

 cleanup:
    PRRTE_RELEASE(op);
}
