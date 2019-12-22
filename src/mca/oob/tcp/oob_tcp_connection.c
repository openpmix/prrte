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
 * Copyright (c) 2009-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies Ltd. All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>
#include <sys/socket.h>

#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_NET_UIO_H
#include <net/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include "src/include/prrte_socket_errno.h"
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#include "types.h"
#include "prrte_stdint.h"
#include "src/mca/backtrace/backtrace.h"
#include "src/mca/base/prrte_mca_base_var.h"
#include "src/util/output.h"
#include "src/util/net.h"
#include "src/util/fd.h"
#include "src/util/error.h"
#include "src/util/show_help.h"
#include "src/class/prrte_hash_table.h"
#include "src/event/event-internal.h"

#include "src/util/name_fns.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"
#include "src/mca/state/state.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/routed/routed.h"
#include "src/runtime/prrte_wait.h"

#include "oob_tcp.h"
#include "src/mca/oob/tcp/oob_tcp_component.h"
#include "src/mca/oob/tcp/oob_tcp_peer.h"
#include "src/mca/oob/tcp/oob_tcp_common.h"
#include "src/mca/oob/tcp/oob_tcp_connection.h"
#include "oob_tcp_peer.h"
#include "oob_tcp_common.h"
#include "oob_tcp_connection.h"

static void tcp_peer_event_init(prrte_oob_tcp_peer_t* peer);
static int  tcp_peer_send_connect_ack(prrte_oob_tcp_peer_t* peer);
static int tcp_peer_send_connect_nack(int sd, prrte_process_name_t name);
static int tcp_peer_send_blocking(int sd, void* data, size_t size);
static bool tcp_peer_recv_blocking(prrte_oob_tcp_peer_t* peer, int sd,
                                   void* data, size_t size);
static void tcp_peer_connected(prrte_oob_tcp_peer_t* peer);

static int tcp_peer_create_socket(prrte_oob_tcp_peer_t* peer, sa_family_t family)
{
    int flags;

    if (peer->sd >= 0) {
        return PRRTE_SUCCESS;
    }

    PRRTE_OUTPUT_VERBOSE((1, prrte_oob_base_framework.framework_output,
                         "%s oob:tcp:peer creating socket to %s",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                         PRRTE_NAME_PRINT(&(peer->name))));
    peer->sd = socket(family, SOCK_STREAM, 0);
    if (peer->sd < 0) {
        prrte_output(0, "%s-%s tcp_peer_create_socket: socket() failed: %s (%d)\n",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                    PRRTE_NAME_PRINT(&(peer->name)),
                    strerror(prrte_socket_errno),
                    prrte_socket_errno);
        return PRRTE_ERR_UNREACH;
    }

    /* Set this fd to be close-on-exec so that any subsequent children don't see it */
    if (prrte_fd_set_cloexec(peer->sd) != PRRTE_SUCCESS) {
        prrte_output(0, "%s unable to set socket to CLOEXEC",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        close(peer->sd);
        peer->sd = -1;
        return PRRTE_ERROR;
    }

    /* setup socket options */
    prrte_oob_tcp_set_socket_options(peer->sd);

    /* setup event callbacks */
    tcp_peer_event_init(peer);

    /* setup the socket as non-blocking */
    if (peer->sd >= 0) {
        if((flags = fcntl(peer->sd, F_GETFL, 0)) < 0) {
            prrte_output(0, "%s-%s tcp_peer_connect: fcntl(F_GETFL) failed: %s (%d)\n",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&(peer->name)),
                        strerror(prrte_socket_errno),
                        prrte_socket_errno);
        } else {
            flags |= O_NONBLOCK;
            if(fcntl(peer->sd, F_SETFL, flags) < 0)
                prrte_output(0, "%s-%s tcp_peer_connect: fcntl(F_SETFL) failed: %s (%d)\n",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&(peer->name)),
                            strerror(prrte_socket_errno),
                            prrte_socket_errno);
        }
    }

    return PRRTE_SUCCESS;
}


/*
 * Try connecting to a peer - cycle across all known addresses
 * until one succeeds.
 */
void prrte_oob_tcp_peer_try_connect(int fd, short args, void *cbdata)
{
    prrte_oob_tcp_conn_op_t *op = (prrte_oob_tcp_conn_op_t*)cbdata;
    prrte_oob_tcp_peer_t *peer;
    int current_socket_family = 0;
    int rc;
    prrte_socklen_t addrlen = 0;
    prrte_oob_tcp_addr_t *addr;
    char *host;
    prrte_oob_tcp_send_t *snd;
    bool connected = false;

    PRRTE_ACQUIRE_OBJECT(op);
    peer = op->peer;

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s prrte_tcp_peer_try_connect: "
                        "attempting to connect to proc %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&(peer->name)));

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s prrte_tcp_peer_try_connect: "
                        "attempting to connect to proc %s on socket %d",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&(peer->name)), peer->sd);

    peer->active_addr = NULL;
    PRRTE_LIST_FOREACH(addr, &peer->addrs, prrte_oob_tcp_addr_t) {
        prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                            "%s prrte_tcp_peer_try_connect: "
                            "attempting to connect to proc %s on %s:%d - %d retries",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&(peer->name)),
                            prrte_net_get_hostname((struct sockaddr*)&addr->addr),
                            prrte_net_get_port((struct sockaddr*)&addr->addr),
                            addr->retries);
        if (MCA_OOB_TCP_FAILED == addr->state) {
            prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                                "%s prrte_tcp_peer_try_connect: %s:%d is down",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                prrte_net_get_hostname((struct sockaddr*)&addr->addr),
                                prrte_net_get_port((struct sockaddr*)&addr->addr));
            continue;
        }
        if (prrte_oob_tcp_component.max_retries < addr->retries) {
            prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                                "%s prrte_tcp_peer_try_connect: %s:%d retries exceeded",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                prrte_net_get_hostname((struct sockaddr*)&addr->addr),
                                prrte_net_get_port((struct sockaddr*)&addr->addr));
            continue;
        }
        peer->active_addr = addr;  // record the one we are using
        addrlen = addr->addr.ss_family == AF_INET6 ? sizeof(struct sockaddr_in6)
                                                   : sizeof(struct sockaddr_in);
        if (addr->addr.ss_family != current_socket_family) {
            if (peer->sd >= 0) {
                CLOSE_THE_SOCKET(peer->sd);
                peer->sd = -1;
            }
            rc = tcp_peer_create_socket(peer, addr->addr.ss_family);
            current_socket_family = addr->addr.ss_family;

            if (PRRTE_SUCCESS != rc) {
                /* FIXME: we cannot create a TCP socket - this spans
                 * all interfaces, so all we can do is report
                 * back to the component that this peer is
                 * unreachable so it can remove the peer
                 * from its list and report back to the base
                 * NOTE: this could be a reconnect attempt,
                 * so we also need to mark any queued messages
                 * and return them as "unreachable"
                 */
                prrte_output(0, "%s CANNOT CREATE SOCKET", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
                PRRTE_FORCED_TERMINATE(1);
                goto cleanup;
            }
        }
    retry_connect:
        addr->retries++;

        rc = connect(peer->sd, (struct sockaddr*) &addr->addr, addrlen);
        if (rc < 0) {
            /* non-blocking so wait for completion */
            if (prrte_socket_errno == EINPROGRESS || prrte_socket_errno == EWOULDBLOCK) {
                prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                                    "%s waiting for connect completion to %s - activating send event",
                                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                    PRRTE_NAME_PRINT(&peer->name));
                /* just ensure the send_event is active */
                if (!peer->send_ev_active) {
                    prrte_event_add(&peer->send_event, 0);
                    peer->send_ev_active = true;
                }
                PRRTE_RELEASE(op);
                return;
            }

            /* Some kernels (Linux 2.6) will automatically software
               abort a connection that was ECONNREFUSED on the last
               attempt, without even trying to establish the
               connection.  Handle that case in a semi-rational
               way by trying twice before giving up */
            if (ECONNABORTED == prrte_socket_errno) {
                if (addr->retries < prrte_oob_tcp_component.max_retries) {
                    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                                        "%s connection aborted by OS to %s - retrying",
                                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                        PRRTE_NAME_PRINT(&peer->name));
                    goto retry_connect;
                } else {
                    /* We were unsuccessful in establishing this connection, and are
                     * not likely to suddenly become successful, so rotate to next option
                     */
                    addr->state = MCA_OOB_TCP_FAILED;
                    continue;
                }
            }
        } else {
            /* connection succeeded */
            addr->retries = 0;
            connected = true;
            peer->num_retries = 0;
            break;
        }
    }

    if (!connected) {
        /* it could be that the intended recipient just hasn't
         * started yet. if requested, wait awhile and try again
         * unless/until we hit the maximum number of retries */
        if (0 < prrte_oob_tcp_component.retry_delay) {
            if (prrte_oob_tcp_component.max_recon_attempts < 0 ||
                peer->num_retries < prrte_oob_tcp_component.max_recon_attempts) {
                struct timeval tv;
                /* close the current socket */
                CLOSE_THE_SOCKET(peer->sd);
                /* reset the addr states */
                PRRTE_LIST_FOREACH(addr, &peer->addrs, prrte_oob_tcp_addr_t) {
                    addr->state = MCA_OOB_TCP_UNCONNECTED;
                    addr->retries = 0;
                }
                /* give it awhile and try again */
                tv.tv_sec = prrte_oob_tcp_component.retry_delay;
                tv.tv_usec = 0;
                ++peer->num_retries;
                PRRTE_RETRY_TCP_CONN_STATE(peer, prrte_oob_tcp_peer_try_connect, &tv);
                goto cleanup;
            }
        }
        /* no address succeeded, so we cannot reach this peer */
        peer->state = MCA_OOB_TCP_FAILED;
        host = prrte_get_proc_hostname(&(peer->name));
        if (NULL == host && NULL != peer->active_addr) {
            host = prrte_net_get_hostname((struct sockaddr*)&(peer->active_addr->addr));
        }
        /* use an prrte_output here instead of show_help as we may well
         * not be connected to the HNP at this point */
        prrte_output(prrte_clean_output,
                    "------------------------------------------------------------\n"
                    "A process or daemon was unable to complete a TCP connection\n"
                    "to another process:\n"
                    "  Local host:    %s\n"
                    "  Remote host:   %s\n"
                    "This is usually caused by a firewall on the remote host. Please\n"
                    "check that any firewall (e.g., iptables) has been disabled and\n"
                    "try again.\n"
                    "------------------------------------------------------------",
                    prrte_process_info.nodename,
                    (NULL == host) ? "<unknown>" : host);
        /* close the socket */
        CLOSE_THE_SOCKET(peer->sd);
        /* let the TCP component know that this module failed to make
         * the connection so it can do some bookkeeping and fail back
         * to the OOB level so another component can try. This will activate
         * an event in the component event base, and so it will fire async
         * from us if we are in our own progress thread
         */
        PRRTE_ACTIVATE_TCP_CMP_OP(peer, prrte_oob_tcp_component_failed_to_connect);
        /* FIXME: post any messages in the send queue back to the OOB
         * level for reassignment
         */
        if (NULL != peer->send_msg) {
        }
        while (NULL != (snd = (prrte_oob_tcp_send_t*)prrte_list_remove_first(&peer->send_queue))) {
        }
        goto cleanup;
    }

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s prrte_tcp_peer_try_connect: "
                        "Connection to proc %s succeeded",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&peer->name));

    /* setup our recv to catch the return ack call */
    if (!peer->recv_ev_active) {
        prrte_event_add(&peer->recv_event, 0);
        peer->recv_ev_active = true;
    }

    /* send our globally unique process identifier to the peer */
    if (PRRTE_SUCCESS == (rc = tcp_peer_send_connect_ack(peer))) {
        peer->state = MCA_OOB_TCP_CONNECT_ACK;
    } else if (PRRTE_ERR_UNREACH == rc) {
        /* this could happen if we are in a race condition where both
         * we and the peer are trying to connect at the same time. If I
         * am the higher vpid, then retry the connection - otherwise,
         * step aside for now */
        int cmpval = prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, PRRTE_PROC_MY_NAME, &peer->name);
        if (PRRTE_VALUE1_GREATER == cmpval) {
            peer->state = MCA_OOB_TCP_CONNECTING;
            PRRTE_ACTIVATE_TCP_CONN_STATE(peer, prrte_oob_tcp_peer_try_connect);
        } else {
            peer->state = MCA_OOB_TCP_UNCONNECTED;
        }
        /* close the socket */
        CLOSE_THE_SOCKET(peer->sd);
        return;
    } else {
        prrte_output(0,
                    "%s prrte_tcp_peer_try_connect: "
                    "tcp_peer_send_connect_ack to proc %s on %s:%d failed: %s (%d)",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                    PRRTE_NAME_PRINT(&(peer->name)),
                    prrte_net_get_hostname((struct sockaddr*)&addr->addr),
                    prrte_net_get_port((struct sockaddr*)&addr->addr),
                    prrte_strerror(rc),
                    rc);
        /* close the socket */
        CLOSE_THE_SOCKET(peer->sd);
        PRRTE_FORCED_TERMINATE(1);
    }

 cleanup:
    PRRTE_RELEASE(op);
}

/* send a handshake that includes our process identifier, our
 * version string, and a security token to ensure we are talking
 * to another OMPI process
 */
static int tcp_peer_send_connect_ack(prrte_oob_tcp_peer_t* peer)
{
    char *msg;
    prrte_oob_tcp_hdr_t hdr;
    uint16_t ack_flag = htons(1);
    size_t sdsize, offset = 0;

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s SEND CONNECT ACK", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* load the header */
    hdr.origin = *PRRTE_PROC_MY_NAME;
    hdr.dst = peer->name;
    hdr.type = MCA_OOB_TCP_IDENT;
    hdr.tag = 0;
    hdr.seq_num = 0;
    memset(hdr.routed, 0, PRRTE_MAX_RTD_SIZE+1);

    /* payload size */
    sdsize = sizeof(ack_flag) + strlen(prrte_version_string) + 1;
    hdr.nbytes = sdsize;
    MCA_OOB_TCP_HDR_HTON(&hdr);

    /* create a space for our message */
    sdsize += sizeof(hdr);
    if (NULL == (msg = (char*)malloc(sdsize))) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the message */
    memcpy(msg + offset, &hdr, sizeof(hdr));
    offset += sizeof(hdr);
    memcpy(msg + offset, &ack_flag, sizeof(ack_flag));
    offset += sizeof(ack_flag);
    memcpy(msg + offset, prrte_version_string, strlen(prrte_version_string));
    offset += strlen(prrte_version_string)+1;

    /* send it */
    if (PRRTE_SUCCESS != tcp_peer_send_blocking(peer->sd, msg, sdsize)) {
        free(msg);
        peer->state = MCA_OOB_TCP_FAILED;
        prrte_oob_tcp_peer_close(peer);
        return PRRTE_ERR_UNREACH;
    }
    free(msg);

    return PRRTE_SUCCESS;
}

/* send a handshake that includes our process identifier, our
 * version string, and a security token to ensure we are talking
 * to another OMPI process
 */
static int tcp_peer_send_connect_nack(int sd, prrte_process_name_t name)
{
    char *msg;
    prrte_oob_tcp_hdr_t hdr;
    uint16_t ack_flag = htons(0);
    int rc = PRRTE_SUCCESS;
    size_t sdsize, offset = 0;

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s SEND CONNECT NACK", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));

    /* load the header */
    hdr.origin = *PRRTE_PROC_MY_NAME;
    hdr.dst = name;
    hdr.type = MCA_OOB_TCP_IDENT;
    hdr.tag = 0;
    hdr.seq_num = 0;
    memset(hdr.routed, 0, PRRTE_MAX_RTD_SIZE+1);

    /* payload size */
    sdsize = sizeof(ack_flag);
    hdr.nbytes = sdsize;
    MCA_OOB_TCP_HDR_HTON(&hdr);

    /* create a space for our message */
    sdsize += sizeof(hdr);
    if (NULL == (msg = (char*)malloc(sdsize))) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the message */
    memcpy(msg + offset, &hdr, sizeof(hdr));
    offset += sizeof(hdr);
    memcpy(msg + offset, &ack_flag, sizeof(ack_flag));
    offset += sizeof(ack_flag);

    /* send it */
    if (PRRTE_SUCCESS != tcp_peer_send_blocking(sd, msg, sdsize)) {
        /* it's ok if it fails - remote side may already
         * identifiet the collision and closed the connection
         */
        rc = PRRTE_SUCCESS;
    }
    free(msg);
    return rc;
}

/*
 * Initialize events to be used by the peer instance for TCP select/poll callbacks.
 */
static void tcp_peer_event_init(prrte_oob_tcp_peer_t* peer)
{
    if (peer->sd >= 0) {
        assert(!peer->send_ev_active && !peer->recv_ev_active);
        prrte_event_set(prrte_event_base,
                       &peer->recv_event,
                       peer->sd,
                       PRRTE_EV_READ|PRRTE_EV_PERSIST,
                       prrte_oob_tcp_recv_handler,
                       peer);
        prrte_event_set_priority(&peer->recv_event, PRRTE_MSG_PRI);
        if (peer->recv_ev_active) {
            prrte_event_del(&peer->recv_event);
            peer->recv_ev_active = false;
        }

        prrte_event_set(prrte_event_base,
                       &peer->send_event,
                       peer->sd,
                       PRRTE_EV_WRITE|PRRTE_EV_PERSIST,
                       prrte_oob_tcp_send_handler,
                       peer);
        prrte_event_set_priority(&peer->send_event, PRRTE_MSG_PRI);
        if (peer->send_ev_active) {
            prrte_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }
    }
}

/*
 * Check the status of the connection. If the connection failed, will retry
 * later. Otherwise, send this processes identifier to the peer on the
 * newly connected socket.
 */
void prrte_oob_tcp_peer_complete_connect(prrte_oob_tcp_peer_t *peer)
{
    int so_error = 0;
    prrte_socklen_t so_length = sizeof(so_error);

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s:tcp:complete_connect called for peer %s on socket %d",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&peer->name), peer->sd);

    /* check connect completion status */
    if (getsockopt(peer->sd, SOL_SOCKET, SO_ERROR, (char *)&so_error, &so_length) < 0) {
        prrte_output(0, "%s tcp_peer_complete_connect: getsockopt() to %s failed: %s (%d)\n",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                    PRRTE_NAME_PRINT(&(peer->name)),
                    strerror(prrte_socket_errno),
                    prrte_socket_errno);
        peer->state = MCA_OOB_TCP_FAILED;
        prrte_oob_tcp_peer_close(peer);
        return;
    }

    if (so_error == EINPROGRESS) {
        prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                            "%s:tcp:send:handler still in progress",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        return;
    } else if (so_error == ECONNREFUSED || so_error == ETIMEDOUT) {
        prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                            "%s-%s tcp_peer_complete_connect: connection failed: %s (%d)",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&(peer->name)),
                            strerror(so_error),
                            so_error);
        prrte_oob_tcp_peer_close(peer);
        return;
    } else if (so_error != 0) {
        /* No need to worry about the return code here - we return regardless
           at this point, and if an error did occur a message has already been
           printed for the user */
        prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                            "%s-%s tcp_peer_complete_connect: "
                            "connection failed with error %d",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&(peer->name)), so_error);
        prrte_oob_tcp_peer_close(peer);
        return;
    }

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s tcp_peer_complete_connect: "
                        "sending ack to %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&(peer->name)));

    if (tcp_peer_send_connect_ack(peer) == PRRTE_SUCCESS) {
        peer->state = MCA_OOB_TCP_CONNECT_ACK;
        prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                            "%s tcp_peer_complete_connect: "
                            "setting read event on connection to %s",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&(peer->name)));

        if (!peer->recv_ev_active) {
            peer->recv_ev_active = true;
            PRRTE_POST_OBJECT(peer);
            prrte_event_add(&peer->recv_event, 0);
        }
    } else {
        prrte_output(0, "%s tcp_peer_complete_connect: unable to send connect ack to %s",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                    PRRTE_NAME_PRINT(&(peer->name)));
        peer->state = MCA_OOB_TCP_FAILED;
        prrte_oob_tcp_peer_close(peer);
    }
}

/*
 * A blocking send on a non-blocking socket. Used to send the small amount of connection
 * information that identifies the peers endpoint.
 */
static int tcp_peer_send_blocking(int sd, void* data, size_t size)
{
    unsigned char* ptr = (unsigned char*)data;
    size_t cnt = 0;
    int retval;

    PRRTE_ACQUIRE_OBJECT(ptr);

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s send blocking of %"PRIsize_t" bytes to socket %d",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        size, sd);

    while (cnt < size) {
        retval = send(sd, (char*)ptr+cnt, size-cnt, 0);
        if (retval < 0) {
            if (prrte_socket_errno != EINTR && prrte_socket_errno != EAGAIN && prrte_socket_errno != EWOULDBLOCK) {
                prrte_output(0, "%s tcp_peer_send_blocking: send() to socket %d failed: %s (%d)\n",
                    PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), sd,
                    strerror(prrte_socket_errno),
                    prrte_socket_errno);
                return PRRTE_ERR_UNREACH;
            }
            continue;
        }
        cnt += retval;
    }

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s blocking send complete to socket %d",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), sd);

    return PRRTE_SUCCESS;
}

/*
 *  Receive the peers globally unique process identification from a newly
 *  connected socket and verify the expected response. If so, move the
 *  socket to a connected state.
 */
static bool retry(prrte_oob_tcp_peer_t* peer, int sd, bool fatal)
{
    int cmpval;

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s SIMUL CONNECTION WITH %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&peer->name));
    cmpval = prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, &peer->name, PRRTE_PROC_MY_NAME);
    if (fatal) {
        if (peer->send_ev_active) {
            prrte_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }
        if (peer->recv_ev_active) {
            prrte_event_del(&peer->recv_event);
            peer->recv_ev_active = false;
        }
        if (0 <= peer->sd) {
            CLOSE_THE_SOCKET(peer->sd);
            peer->sd = -1;
        }
        if (PRRTE_VALUE1_GREATER == cmpval) {
            /* force the other end to retry the connection */
            peer->state = MCA_OOB_TCP_UNCONNECTED;
        } else {
            /* retry the connection */
            peer->state = MCA_OOB_TCP_CONNECTING;
            PRRTE_ACTIVATE_TCP_CONN_STATE(peer, prrte_oob_tcp_peer_try_connect);
        }
        return true;
    } else {
        if (PRRTE_VALUE1_GREATER == cmpval) {
            /* The other end will retry the connection */
            if (peer->send_ev_active) {
                prrte_event_del(&peer->send_event);
                peer->send_ev_active = false;
            }
            if (peer->recv_ev_active) {
                prrte_event_del(&peer->recv_event);
                peer->recv_ev_active = false;
            }
            CLOSE_THE_SOCKET(peer->sd);
            peer->state = MCA_OOB_TCP_UNCONNECTED;
            return false;
        } else {
            /* The connection will be retried */
            tcp_peer_send_connect_nack(sd, peer->name);
            CLOSE_THE_SOCKET(sd);
            return true;
        }
    }
}


int prrte_oob_tcp_peer_recv_connect_ack(prrte_oob_tcp_peer_t* pr,
                                      int sd, prrte_oob_tcp_hdr_t *dhdr)
{
    char *msg;
    char *version;
    size_t offset = 0;
    prrte_oob_tcp_hdr_t hdr;
    prrte_oob_tcp_peer_t *peer;
    uint64_t *ui64;
    uint16_t ack_flag;
    bool is_new = (NULL == pr);

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s RECV CONNECT ACK FROM %s ON SOCKET %d",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (NULL == pr) ? "UNKNOWN" : PRRTE_NAME_PRINT(&pr->name), sd);

    peer = pr;
    /* get the header */
    if (tcp_peer_recv_blocking(peer, sd, &hdr, sizeof(prrte_oob_tcp_hdr_t))) {
        if (NULL != peer) {
            /* If the peer state is CONNECT_ACK, then we were waiting for
             * the connection to be ack'd
             */
            if (peer->state != MCA_OOB_TCP_CONNECT_ACK) {
                /* handshake broke down - abort this connection */
                prrte_output(0, "%s RECV CONNECT BAD HANDSHAKE (%d) FROM %s ON SOCKET %d",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME), peer->state,
                            PRRTE_NAME_PRINT(&(peer->name)), sd);
                prrte_oob_tcp_peer_close(peer);
                return PRRTE_ERR_UNREACH;
            }
        }
    } else {
        /* unable to complete the recv */
        prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                            "%s unable to complete recv of connect-ack from %s ON SOCKET %d",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            (NULL == peer) ? "UNKNOWN" : PRRTE_NAME_PRINT(&peer->name), sd);
        return PRRTE_ERR_UNREACH;
    }

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s connect-ack recvd from %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (NULL == peer) ? "UNKNOWN" : PRRTE_NAME_PRINT(&peer->name));

    /* convert the header */
    MCA_OOB_TCP_HDR_NTOH(&hdr);
    /* if the requestor wanted the header returned, then do so now */
    if (NULL != dhdr) {
        *dhdr = hdr;
    }

    if (MCA_OOB_TCP_PROBE == hdr.type) {
        /* send a header back */
        hdr.type = MCA_OOB_TCP_PROBE;
        hdr.dst = hdr.origin;
        hdr.origin = *PRRTE_PROC_MY_NAME;
        MCA_OOB_TCP_HDR_HTON(&hdr);
        tcp_peer_send_blocking(sd, &hdr, sizeof(prrte_oob_tcp_hdr_t));
        CLOSE_THE_SOCKET(sd);
        return PRRTE_SUCCESS;
    }

    if (hdr.type != MCA_OOB_TCP_IDENT) {
        prrte_output(0, "tcp_peer_recv_connect_ack: invalid header type: %d\n",
                    hdr.type);
        if (NULL != peer) {
            peer->state = MCA_OOB_TCP_FAILED;
            prrte_oob_tcp_peer_close(peer);
        } else {
            CLOSE_THE_SOCKET(sd);
        }
        return PRRTE_ERR_COMM_FAILURE;
    }

    /* if we don't already have it, get the peer */
    if (NULL == peer) {
        peer = prrte_oob_tcp_peer_lookup(&hdr.origin);
        if (NULL == peer) {
            prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                                "%s prrte_oob_tcp_recv_connect: connection from new peer",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
            peer = PRRTE_NEW(prrte_oob_tcp_peer_t);
            peer->name = hdr.origin;
            peer->state = MCA_OOB_TCP_ACCEPTING;
            ui64 = (uint64_t*)(&peer->name);
            if (PRRTE_SUCCESS != prrte_hash_table_set_value_uint64(&prrte_oob_tcp_component.peers, (*ui64), peer)) {
                PRRTE_RELEASE(peer);
                CLOSE_THE_SOCKET(sd);
                return PRRTE_ERR_OUT_OF_RESOURCE;
            }
        }
    } else {
        /* compare the peers name to the expected value */
        if (PRRTE_EQUAL != prrte_util_compare_name_fields(PRRTE_NS_CMP_ALL, &peer->name, &hdr.origin)) {
            prrte_output(0, "%s tcp_peer_recv_connect_ack: "
                        "received unexpected process identifier %s from %s\n",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&(hdr.origin)),
                        PRRTE_NAME_PRINT(&(peer->name)));
            peer->state = MCA_OOB_TCP_FAILED;
            prrte_oob_tcp_peer_close(peer);
            return PRRTE_ERR_CONNECTION_REFUSED;
        }
    }

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s connect-ack header from %s is okay",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&peer->name));

    /* get the authentication and version payload */
    if (NULL == (msg = (char*)malloc(hdr.nbytes))) {
        peer->state = MCA_OOB_TCP_FAILED;
        prrte_oob_tcp_peer_close(peer);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    if (!tcp_peer_recv_blocking(peer, sd, msg, hdr.nbytes)) {
        /* unable to complete the recv but should never happen */
        prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                            "%s unable to complete recv of connect-ack from %s ON SOCKET %d",
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                            PRRTE_NAME_PRINT(&peer->name), peer->sd);
        free(msg);
        return PRRTE_ERR_UNREACH;
    }

    /* Check the type of acknowledgement */
    memcpy(&ack_flag, msg + offset, sizeof(ack_flag));
    offset += sizeof(ack_flag);

    ack_flag = ntohs(ack_flag);
    if( !ack_flag ){
        if (MCA_OOB_TCP_CONNECT_ACK == peer->state) {
            /* We got nack from the remote side which means that
             * it will be the initiator of the connection.
             */

            /* release the socket */
            CLOSE_THE_SOCKET(peer->sd);
            peer->sd = -1;

            /* unregister active events */
            if (peer->recv_ev_active) {
                prrte_event_del(&peer->recv_event);
                peer->recv_ev_active = false;
            }
            if (peer->send_ev_active) {
                prrte_event_del(&peer->send_event);
                peer->send_ev_active = false;
            }

            /* change the state so we'll accept the remote
             * connection when it'll apeear
             */
            peer->state = MCA_OOB_TCP_UNCONNECTED;
        } else {
            /* FIXME: this shouldn't happen. We need to force next address
             * to be tried.
             */
            prrte_oob_tcp_peer_close(peer);
        }
        free(msg);
        return PRRTE_ERR_UNREACH;
    }

    /* check for a race condition - if I was in the process of
     * creating a connection to the peer, or have already established
     * such a connection, then we need to reject this connection. We will
     * let the higher ranked process retry - if I'm the lower ranked
     * process, I'll simply defer until I receive the request
     */
    if (is_new &&
        ( MCA_OOB_TCP_CONNECTED == peer->state ||
          MCA_OOB_TCP_CONNECTING == peer->state ||
         MCA_OOB_TCP_CONNECT_ACK == peer->state ) ) {
        if (retry(peer, sd, false)) {
            free(msg);
            return PRRTE_ERR_UNREACH;
        }
    }

    /* check that this is from a matching version */
    version = (char*)((char*)msg + offset);
    offset += strlen(version) + 1;
    if (0 != strcmp(version, prrte_version_string)) {
        prrte_show_help("help-oob-tcp.txt", "version mismatch",
                       true,
                       prrte_process_info.nodename,
                       PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                       prrte_version_string,
                       prrte_fd_get_peer_name(peer->sd),
                       PRRTE_NAME_PRINT(&(peer->name)),
                       version);

        peer->state = MCA_OOB_TCP_FAILED;
        prrte_oob_tcp_peer_close(peer);
        free(msg);
        return PRRTE_ERR_CONNECTION_REFUSED;
    }
    free(msg);

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s connect-ack version from %s matches ours",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&peer->name));

    /* if the requestor wanted the header returned, then they
     * will complete their processing
     */
    if (NULL != dhdr) {
        return PRRTE_SUCCESS;
    }

    /* set the peer into the component and OOB-level peer tables to indicate
     * that we know this peer and we will be handling him
     */
    PRRTE_ACTIVATE_TCP_CMP_OP(peer, prrte_oob_tcp_component_set_module);

    /* connected */
    tcp_peer_connected(peer);
    if (OOB_TCP_DEBUG_CONNECT <= prrte_output_get_verbosity(prrte_oob_base_framework.framework_output)) {
        prrte_oob_tcp_peer_dump(peer, "connected");
    }
    return PRRTE_SUCCESS;
}

/*
 *  Setup peer state to reflect that connection has been established,
 *  and start any pending sends.
 */
static void tcp_peer_connected(prrte_oob_tcp_peer_t* peer)
{
    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s-%s tcp_peer_connected on socket %d",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&(peer->name)), peer->sd);

    if (peer->timer_ev_active) {
        prrte_event_del(&peer->timer_event);
        peer->timer_ev_active = false;
    }
    peer->state = MCA_OOB_TCP_CONNECTED;
    if (NULL != peer->active_addr) {
        peer->active_addr->retries = 0;
    }

    /* update the route */
    prrte_routed.update_route(&peer->name, &peer->name);

    /* initiate send of first message on queue */
    if (NULL == peer->send_msg) {
        peer->send_msg = (prrte_oob_tcp_send_t*)
            prrte_list_remove_first(&peer->send_queue);
    }
    if (NULL != peer->send_msg && !peer->send_ev_active) {
        peer->send_ev_active = true;
        PRRTE_POST_OBJECT(peer);
        prrte_event_add(&peer->send_event, 0);
    }
}

/*
 * Remove any event registrations associated with the socket
 * and update the peer state to reflect the connection has
 * been closed.
 */
void prrte_oob_tcp_peer_close(prrte_oob_tcp_peer_t *peer)
{
    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s tcp_peer_close for %s sd %d state %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&(peer->name)),
                        peer->sd, prrte_oob_tcp_state_print(peer->state));

    /* release the socket */
    close(peer->sd);
    peer->sd = -1;

    /* if we were CONNECTING, then we need to mark the address as
     * failed and cycle back to try the next address */
    if (MCA_OOB_TCP_CONNECTING == peer->state) {
        if (NULL != peer->active_addr) {
            peer->active_addr->state = MCA_OOB_TCP_FAILED;
        }
        PRRTE_ACTIVATE_TCP_CONN_STATE(peer, prrte_oob_tcp_peer_try_connect);
        return;
    }

    peer->state = MCA_OOB_TCP_CLOSED;
    if (NULL != peer->active_addr) {
        peer->active_addr->state = MCA_OOB_TCP_CLOSED;
    }

    /* unregister active events */
    if (peer->recv_ev_active) {
        prrte_event_del(&peer->recv_event);
        peer->recv_ev_active = false;
    }
    if (peer->send_ev_active) {
        prrte_event_del(&peer->send_event);
        peer->send_ev_active = false;
    }

    /* inform the component-level that we have lost a connection so
     * it can decide what to do about it.
     */
    PRRTE_ACTIVATE_TCP_CMP_OP(peer, prrte_oob_tcp_component_lost_connection);

    if (prrte_prteds_term_ordered || prrte_finalizing || prrte_abnormal_term_ordered) {
        /* nothing more to do */
        return;
    }

    /* FIXME: push any queued messages back onto the OOB for retry - note that
     * this must be done after the prior call to ensure that the component
     * processes the "lost connection" notice before the OOB begins to
     * handle these recycled messages. This prevents us from unintentionally
     * attempting to send the message again across the now-failed interface
     */
    /*
    if (NULL != peer->send_msg) {
    }
    while (NULL != (snd = (prrte_oob_tcp_send_t*)prrte_list_remove_first(&peer->send_queue))) {
    }
    */
}

/*
 * A blocking recv on a non-blocking socket. Used to receive the small amount of connection
 * information that identifies the peers endpoint.
 */
static bool tcp_peer_recv_blocking(prrte_oob_tcp_peer_t* peer, int sd,
                                   void* data, size_t size)
{
    unsigned char* ptr = (unsigned char*)data;
    size_t cnt = 0;

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s waiting for connect ack from %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (NULL == peer) ? "UNKNOWN" : PRRTE_NAME_PRINT(&(peer->name)));

    while (cnt < size) {
        int retval = recv(sd, (char *)ptr+cnt, size-cnt, 0);

        /* remote closed connection */
        if (retval == 0) {
            prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                                "%s-%s tcp_peer_recv_blocking: "
                                "peer closed connection: peer state %d",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                (NULL == peer) ? "UNKNOWN" : PRRTE_NAME_PRINT(&(peer->name)),
                                (NULL == peer) ? 0 : peer->state);
            if (NULL != peer) {
                prrte_oob_tcp_peer_close(peer);
            } else {
                CLOSE_THE_SOCKET(sd);
            }
            return false;
        }

        /* socket is non-blocking so handle errors */
        if (retval < 0) {
            if (prrte_socket_errno != EINTR &&
                prrte_socket_errno != EAGAIN &&
                prrte_socket_errno != EWOULDBLOCK) {
                if (NULL == peer) {
                    /* protect against things like port scanners */
                    CLOSE_THE_SOCKET(sd);
                    return false;
                } else if (peer->state == MCA_OOB_TCP_CONNECT_ACK) {
                    /* If we overflow the listen backlog, it's
                       possible that even though we finished the three
                       way handshake, the remote host was unable to
                       transition the connection from half connected
                       (received the initial SYN) to fully connected
                       (in the listen backlog).  We likely won't see
                       the failure until we try to receive, due to
                       timing and the like.  The first thing we'll get
                       in that case is a RST packet, which receive
                       will turn into a connection reset by peer
                       errno.  In that case, leave the socket in
                       CONNECT_ACK and propogate the error up to
                       recv_connect_ack, who will try to establish the
                       connection again */
                    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                                        "%s connect ack received error %s from %s",
                                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                        strerror(prrte_socket_errno),
                                        PRRTE_NAME_PRINT(&(peer->name)));
                    return false;
                } else {
                    prrte_output(0,
                                "%s tcp_peer_recv_blocking: "
                                "recv() failed for %s: %s (%d)\n",
                                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                                PRRTE_NAME_PRINT(&(peer->name)),
                                strerror(prrte_socket_errno),
                                prrte_socket_errno);
                    peer->state = MCA_OOB_TCP_FAILED;
                    prrte_oob_tcp_peer_close(peer);
                    return false;
                }
            }
            continue;
        }
        cnt += retval;
    }

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s connect ack received from %s",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        (NULL == peer) ? "UNKNOWN" : PRRTE_NAME_PRINT(&(peer->name)));
    return true;
}

/*
 * Routine for debugging to print the connection state and socket options
 */
void prrte_oob_tcp_peer_dump(prrte_oob_tcp_peer_t* peer, const char* msg)
{
    char src[64];
    char dst[64];
    char buff[255];
    int sndbuf,rcvbuf,nodelay,flags;
    struct sockaddr_storage inaddr;
    prrte_socklen_t addrlen = sizeof(struct sockaddr_storage);
    prrte_socklen_t optlen;

    if (getsockname(peer->sd, (struct sockaddr*)&inaddr, &addrlen) < 0) {
        prrte_output(0, "tcp_peer_dump: getsockname: %s (%d)\n",
                    strerror(prrte_socket_errno),
                    prrte_socket_errno);
    } else {
        snprintf(src, sizeof(src), "%s", prrte_net_get_hostname((struct sockaddr*) &inaddr));
    }
    if (getpeername(peer->sd, (struct sockaddr*)&inaddr, &addrlen) < 0) {
        prrte_output(0, "tcp_peer_dump: getpeername: %s (%d)\n",
                    strerror(prrte_socket_errno),
                    prrte_socket_errno);
    } else {
        snprintf(dst, sizeof(dst), "%s", prrte_net_get_hostname((struct sockaddr*) &inaddr));
    }

    if ((flags = fcntl(peer->sd, F_GETFL, 0)) < 0) {
        prrte_output(0, "tcp_peer_dump: fcntl(F_GETFL) failed: %s (%d)\n",
                    strerror(prrte_socket_errno),
                    prrte_socket_errno);
    }

#if defined(SO_SNDBUF)
    optlen = sizeof(sndbuf);
    if(getsockopt(peer->sd, SOL_SOCKET, SO_SNDBUF, (char *)&sndbuf, &optlen) < 0) {
        prrte_output(0, "tcp_peer_dump: SO_SNDBUF option: %s (%d)\n",
                    strerror(prrte_socket_errno),
                    prrte_socket_errno);
    }
#else
    sndbuf = -1;
#endif
#if defined(SO_RCVBUF)
    optlen = sizeof(rcvbuf);
    if (getsockopt(peer->sd, SOL_SOCKET, SO_RCVBUF, (char *)&rcvbuf, &optlen) < 0) {
        prrte_output(0, "tcp_peer_dump: SO_RCVBUF option: %s (%d)\n",
                    strerror(prrte_socket_errno),
                    prrte_socket_errno);
    }
#else
    rcvbuf = -1;
#endif
#if defined(TCP_NODELAY)
    optlen = sizeof(nodelay);
    if (getsockopt(peer->sd, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, &optlen) < 0) {
        prrte_output(0, "tcp_peer_dump: TCP_NODELAY option: %s (%d)\n",
                    strerror(prrte_socket_errno),
                    prrte_socket_errno);
    }
#else
    nodelay = 0;
#endif

    snprintf(buff, sizeof(buff), "%s-%s %s: %s - %s nodelay %d sndbuf %d rcvbuf %d flags %08x\n",
        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
        PRRTE_NAME_PRINT(&(peer->name)),
        msg, src, dst, nodelay, sndbuf, rcvbuf, flags);
    prrte_output(0, "%s", buff);
}

/*
 * Accept incoming connection - if not already connected
 */

bool prrte_oob_tcp_peer_accept(prrte_oob_tcp_peer_t* peer)
{
    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s tcp:peer_accept called for peer %s in state %s on socket %d",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&peer->name),
                        prrte_oob_tcp_state_print(peer->state), peer->sd);

    if (peer->state != MCA_OOB_TCP_CONNECTED) {

        tcp_peer_event_init(peer);

        if (tcp_peer_send_connect_ack(peer) != PRRTE_SUCCESS) {
            prrte_output(0, "%s-%s tcp_peer_accept: "
                        "tcp_peer_send_connect_ack failed\n",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&(peer->name)));
            peer->state = MCA_OOB_TCP_FAILED;
            prrte_oob_tcp_peer_close(peer);
            return false;
        }

        /* set the peer into the component and OOB-level peer tables to indicate
         * that we know this peer and we will be handling him
         */
        PRRTE_ACTIVATE_TCP_CMP_OP(peer, prrte_oob_tcp_component_set_module);

        tcp_peer_connected(peer);
        if (!peer->recv_ev_active) {
            peer->recv_ev_active = true;
            PRRTE_POST_OBJECT(peer);
            prrte_event_add(&peer->recv_event, 0);
        }
        if (OOB_TCP_DEBUG_CONNECT <= prrte_output_get_verbosity(prrte_oob_base_framework.framework_output)) {
            prrte_oob_tcp_peer_dump(peer, "accepted");
        }
        return true;
    }

    prrte_output_verbose(OOB_TCP_DEBUG_CONNECT, prrte_oob_base_framework.framework_output,
                        "%s tcp:peer_accept ignored for peer %s in state %s on socket %d",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                        PRRTE_NAME_PRINT(&peer->name),
                        prrte_oob_tcp_state_print(peer->state), peer->sd);
    return false;
}
