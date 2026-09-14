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
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2020      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <fcntl.h>
#include <sys/socket.h>

#ifdef HAVE_SYS_UIO_H
#    include <sys/uio.h>
#endif
#ifdef HAVE_NET_UIO_H
#    include <net/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include "src/include/prte_socket_errno.h"
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#ifdef HAVE_NETINET_TCP_H
#    include <netinet/tcp.h>
#endif

#include "prte_stdint.h"
#include "src/event/event-internal.h"
#include "src/mca/prtebacktrace/prtebacktrace.h"
#include "src/util/error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"
#include "types.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/prtereachable/base/base.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"

#include "src/rml/oob/oob_tcp.h"
#include "src/rml/oob/oob_tcp_common.h"
#include "src/rml/oob/oob_tcp_connection.h"
#include "src/rml/oob/oob_tcp_peer.h"

static void tcp_peer_event_init(prte_oob_tcp_peer_t *peer);
static int tcp_peer_send_connect_ack(prte_oob_tcp_peer_t *peer);
static int tcp_peer_send_connect_nack(int sd, pmix_proc_t *name);
static int tcp_peer_send_blocking(int sd, void *data, size_t size);
static int tcp_peer_read_handshake(int sd, prte_oob_tcp_handshake_t *hs);
static void tcp_peer_connected(prte_oob_tcp_peer_t *peer);

static int tcp_peer_create_socket(prte_oob_tcp_peer_t *peer, sa_family_t family)
{
    int flags;

    if (peer->sd >= 0) {
        return PRTE_SUCCESS;
    }

    PMIX_OUTPUT_VERBOSE((1, prte_oob_base.output,
                         "%s oob:tcp:peer creating socket to %s",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name))));
    peer->sd = socket(family, SOCK_STREAM, 0);
    if (peer->sd < 0) {
        pmix_output(0, "%s-%s tcp_peer_create_socket: socket() failed: %s (%d)\n",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                    strerror(prte_socket_errno), prte_socket_errno);
        return PRTE_ERR_UNREACH;
    }

    /* Set this fd to be close-on-exec so that any subsequent children don't see it */
    if (pmix_fd_set_cloexec(peer->sd) != PRTE_SUCCESS) {
        pmix_output(0, "%s unable to set socket to CLOEXEC", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        close(peer->sd);
        peer->sd = -1;
        return PRTE_ERROR;
    }

    /* setup socket options */
    prte_oob_tcp_set_socket_options(peer->sd);

    /* a new socket gets a new handshake - forget whatever part of a reply
     * the last one had received */
    prte_oob_tcp_handshake_reset(&peer->hshake);

    /* setup event callbacks */
    tcp_peer_event_init(peer);

    /* setup the socket as non-blocking */
    if (peer->sd >= 0) {
        if ((flags = fcntl(peer->sd, F_GETFL, 0)) < 0) {
            pmix_output(0, "%s-%s tcp_peer_connect: fcntl(F_GETFL) failed: %s (%d)\n",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                        strerror(prte_socket_errno), prte_socket_errno);
        } else {
            flags |= O_NONBLOCK;
            if (fcntl(peer->sd, F_SETFL, flags) < 0) {
                pmix_output(0, "%s-%s tcp_peer_connect: fcntl(F_SETFL) failed: %s (%d)\n",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                            strerror(prte_socket_errno), prte_socket_errno);
            }
        }
    }

    return PRTE_SUCCESS;
}

/*
 * Abandon a handshake that cannot be completed.
 *
 * `sd` is the socket the handshake is running on and `peer` is the peer it
 * claims to belong to - and those are not always the same connection.  On the
 * inbound path recv_handler hands us a socket it has just accepted and only
 * publishes it as peer->sd once the handshake has succeeded, so until then
 * peer->sd is whatever else that peer has: nothing at all, an outbound
 * attempt of our own, or a working connection carrying traffic.  Closing the
 * peer while holding a socket it does not own would tear that down - failing
 * every message queued on it and reporting a lost connection - on the word of
 * a socket that never proved it was the peer, and would leak the accepted
 * socket besides, since recv_handler's cleanup only releases its conn op.
 *
 * So the peer is closed only when this really is its socket, which is also
 * what rotates it onto the next address in its list; otherwise the failure
 * costs nothing but the socket it arrived on.  Ownership is asked of the peer
 * rather than inferred from which direction the handshake came in, because
 * the simultaneous-connect arbitration in retry() can hand an accepted socket
 * to the peer part way through.
 */
static void abort_handshake(prte_oob_tcp_peer_t *peer, int sd)
{
    if (NULL == peer || sd != peer->sd) {
        CLOSE_THE_SOCKET(sd);
        return;
    }
    peer->state = MCA_OOB_TCP_FAILED;
    prte_oob_tcp_peer_close(peer);
}

/*
 * Nothing queued for a peer we are giving up on can ever go out, so finish
 * each of those sends with the status that says why.  Completing them -
 * rather than dropping them on the floor - is what lets the originator learn
 * the message died; RELM in particular is waiting on exactly that callback,
 * and PMIX_RELEASE on the send would free the buffer and tell nobody.
 *
 * Lift them off the peer in one guarded step, since a message can still be
 * queued onto it from another thread, and complete them outside the lock,
 * since completion runs the originator's callback.
 */
static void tcp_peer_lift_queued_sends(prte_oob_tcp_peer_t *peer, pmix_list_t *doomed)
{
    prte_oob_tcp_send_t *snd;

    pmix_mutex_lock(&peer->lock);
    /* the on-deck message is not in the send queue, so it has to be
     * collected separately */
    if (NULL != peer->send_msg) {
        pmix_list_append(doomed, &peer->send_msg->super);
        peer->send_msg = NULL;
    }
    while (NULL != (snd = (prte_oob_tcp_send_t *) pmix_list_remove_first(&peer->send_queue))) {
        pmix_list_append(doomed, &snd->super);
    }
    pmix_mutex_unlock(&peer->lock);
}

static void tcp_peer_fail_queued_sends(prte_oob_tcp_peer_t *peer, int status)
{
    prte_oob_tcp_send_t *snd;
    pmix_list_t doomed;

    PMIX_CONSTRUCT(&doomed, pmix_list_t);
    tcp_peer_lift_queued_sends(peer, &doomed);
    while (NULL != (snd = (prte_oob_tcp_send_t *) pmix_list_remove_first(&doomed))) {
        if (NULL != snd->msg) {
            prte_rml_send_t *m = snd->msg;
            m->status = status;
            snd->msg = NULL; // the completion owns it now
            PRTE_OOB_COMPLETE_SEND(peer, m);
        }
        PMIX_RELEASE(snd);
    }
    PMIX_DESTRUCT(&doomed);
}

/* The sends a lost connection strands, on their way to the main progress
 * thread to be failed there - see tcp_peer_fail_lost_sends() */
typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    pmix_rank_t rank;
    pmix_list_t sends;
} tcp_lost_sends_t;
static void lost_cons(tcp_lost_sends_t *p)
{
    p->rank = PMIX_RANK_INVALID;
    PMIX_CONSTRUCT(&p->sends, pmix_list_t);
}
static void lost_des(tcp_lost_sends_t *p)
{
    PMIX_LIST_DESTRUCT(&p->sends);
}
static PMIX_CLASS_INSTANCE(tcp_lost_sends_t, pmix_object_t, lost_cons, lost_des);

static void tcp_peer_complete_lost_sends(int fd, short args, void *cbdata)
{
    tcp_lost_sends_t *cd = (tcp_lost_sends_t *) cbdata;
    prte_oob_tcp_send_t *snd;
    int status;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(cd);
    status = prte_rml_is_node_up(cd->rank) ? PRTE_ERR_UNREACH : PRTE_ERR_NODE_DOWN;
    while (NULL != (snd = (prte_oob_tcp_send_t *) pmix_list_remove_first(&cd->sends))) {
        if (NULL != snd->msg) {
            prte_rml_send_t *m = snd->msg;
            m->status = status;
            snd->msg = NULL; // the completion owns it now
            PRTE_RML_SEND_COMPLETE(m);
        }
        PMIX_RELEASE(snd);
    }
    PMIX_RELEASE(cd);
}

/*
 * Fail what a lost connection leaves queued.  The status says whether the
 * routing layer already counts the peer's node as down - NODE_DOWN, which the
 * default send callback lets pass quietly - or not - UNREACH, which it
 * reports.  That is a question about the routing tree, and the routing tree is
 * the main progress thread's: prte_rml_is_node_up() reads a bitmap that a
 * DVM resize reallocates.  A lost connection is noticed by whichever thread
 * services the socket, so the sends are lifted here and the question is asked
 * over there.  Nothing about the answer changes by waiting: the loss report
 * that would mark the node down is posted after this, to the same queue.
 */
static void tcp_peer_fail_lost_sends(prte_oob_tcp_peer_t *peer)
{
    tcp_lost_sends_t *cd;

    cd = PMIX_NEW(tcp_lost_sends_t);
    cd->rank = peer->name.rank;
    tcp_peer_lift_queued_sends(peer, &cd->sends);
    if (prte_event_base == peer->evbase) {
        /* no worker threads: we are the main progress thread */
        tcp_peer_complete_lost_sends(-1, 0, cd);
    } else {
        PRTE_PMIX_THREADSHIFT(cd, prte_event_base, tcp_peer_complete_lost_sends);
    }
}

/*
 * Try connecting to a peer - cycle across all known addresses
 * until one succeeds.
 */
void prte_oob_tcp_peer_try_connect(int fd, short args, void *cbdata)
{
    pmix_list_t *local_list = &prte_oob_base.local_ifs, *remote_list;
    int rc, i, j, local_if_count, remote_if_count, best, best_i = 0, best_j = 0;
    prte_oob_tcp_conn_op_t *op = (prte_oob_tcp_conn_op_t *) cbdata;
    prte_reachable_t *results = NULL;
    volatile pmix_list_item_t *ptr;
    prte_socklen_t addrlen = 0;
    prte_oob_tcp_peer_t *peer;
    prte_oob_tcp_addr_t *addr;
    bool connected = false;
    pmix_pif_t *intf;
    char *host;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_ACQUIRE_OBJECT(op);
    peer = op->peer;

    /* Dial only a peer whose connection is being driven - every path that
     * schedules this sets CONNECTING first.  A retry parked on a timer
     * outlives the attempt that parked it, and by the time it fires the peer
     * may have come up another way: most often by dialing us, which retry()
     * lets win the simultaneous-connect arbitration, or by nacking us so that
     * it does the dialing.  Carrying on would close the socket that
     * connection lives on - while its events are armed, possibly on a worker
     * base - to open one of our own. */
    if (MCA_OOB_TCP_CONNECTING != peer->state) {
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s prte_tcp_peer_try_connect: %s is %s - not dialing",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name),
                            prte_oob_tcp_state_print(peer->state));
        PMIX_RELEASE(op);
        return;
    }

    remote_list = PMIX_NEW(pmix_list_t);
    if (NULL == remote_list) {
        pmix_output(0, "%s CANNOT CREATE SOCKET, OUT OF MEMORY",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        tcp_peer_fail_queued_sends(peer, PRTE_ERR_UNREACH);
        PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_COMM_FAILED);
        PMIX_RELEASE(op);
        return;
    }

    /* Construct a list of remote pmix_pif_t from peer */
    PMIX_LIST_FOREACH(addr, &peer->addrs, prte_oob_tcp_addr_t)
    {
        intf = PMIX_NEW(pmix_pif_t);
        if (NULL == intf) {
            pmix_output(0, "%s CANNOT CREATE SOCKET, OUT OF MEMORY",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
            tcp_peer_fail_queued_sends(peer, PRTE_ERR_UNREACH);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_COMM_FAILED);
            goto cleanup;
        }
        intf->af_family = addr->addr.ss_family;
        memcpy(&intf->if_addr, &addr->addr, sizeof(struct sockaddr_storage));
        intf->if_mask = addr->if_mask;
        /* We do not pass along bandwidth information, setting as arbitrary non
         * zero value
         */
        intf->if_bandwidth = 1;
        pmix_list_append(remote_list, &(intf->super));
    }
    local_if_count = pmix_list_get_size(local_list);
    remote_if_count = pmix_list_get_size(remote_list);

    /* NULL only when the matrix could not be allocated; the loop below then
     * finds no route, which fails or retries the peer like any other */
    results = prte_reachable.reachable(local_list, remote_list);

    /* Find match, bind socket. If connect attempt failed, move to next */
    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s prte_tcp_peer_try_connect: "
                        "attempting to connect to proc %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)));

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s prte_tcp_peer_try_connect: "
                        "attempting to connect to proc %s on socket %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                        peer->sd);

    /* Loops over the reachable bitmap. This should only run once, but
     * if a connection does fail even after being declared as reachable,
     * it will try remaining connections.
     */
    while (!connected && NULL != results) {
        /* Select the best connection. This is not going to be a large
         * table and should only run once in the normal case, so no sorting
         * is attempted.
         */
        best = 0;
        for (i = 0; i < local_if_count; i++) {
            for (j = 0; j < remote_if_count; j++) {
                if (best < results->weights[i][j]) {
                    best = results->weights[i][j];
                    best_i = i;
                    best_j = j;
                }
            }
        }
        /* If no connections are found, skip the rest of the connecting logic
         * and exit the loop.
         */
        if (0 == best) {
            break;
        }
        /* Set this entry to be 0 so it won't be selected when looking for
         * the next best connection
         */
        results->weights[best_i][best_j] = 0;
        ptr = peer->addrs.pmix_list_sentinel.pmix_list_next;
        for (j = 0; j < best_j; j++) {
            ptr = ptr->pmix_list_next;
        }
        /* Record the peer address we are using */
        peer->active_addr = (prte_oob_tcp_addr_t *) ptr;
        addr = peer->active_addr;
        /* Grab the local address we are using to bind the socket with */
        ptr = prte_oob_base.local_ifs.pmix_list_sentinel.pmix_list_next;
        for (i = 0; i < best_i; i++) {
            ptr = ptr->pmix_list_next;
        }
        intf = (pmix_pif_t *) ptr;
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s prte_tcp_peer_try_connect: "
                            "attempting to connect to proc %s on %s:%d - %d retries",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                            pmix_net_get_hostname((struct sockaddr *) &addr->addr),
                            pmix_net_get_port((struct sockaddr *) &addr->addr), addr->retries);
        if (MCA_OOB_TCP_FAILED == addr->state) {
            pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                "%s prte_tcp_peer_try_connect: %s:%d is down",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                pmix_net_get_hostname((struct sockaddr *) &addr->addr),
                                pmix_net_get_port((struct sockaddr *) &addr->addr));
            continue;
        }
        if (prte_oob_base.max_retries < addr->retries) {
            pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                "%s prte_tcp_peer_try_connect: %s:%d retries exceeded",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                pmix_net_get_hostname((struct sockaddr *) &addr->addr),
                                pmix_net_get_port((struct sockaddr *) &addr->addr));
            continue;
        }
        addrlen = addr->addr.ss_family == AF_INET6 ? sizeof(struct sockaddr_in6)
                                                   : sizeof(struct sockaddr_in);

        /* Since we are manually binding sockets now, we must
         * close and create a new socket if we are binding to a
         * new address.
         */
        if (peer->sd >= 0) {
            CLOSE_THE_SOCKET(peer->sd);
            peer->sd = -1;
        }
        rc = tcp_peer_create_socket(peer, addr->addr.ss_family);

        if (PRTE_SUCCESS != rc) {
            /* we cannot create a TCP socket - this spans all interfaces, so
             * there is no other address to try and this peer is unreachable.
             * This is a reconnect path as well as a first-connect one, so
             * what is queued on the peer can be real work rather than a
             * handshake: it has to be completed as unreachable, not dropped.
             */
            pmix_output(0, "%s CANNOT CREATE SOCKET", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
            tcp_peer_fail_queued_sends(peer, PRTE_ERR_UNREACH);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_COMM_FAILED);
            goto cleanup;
        }

        /* Bind the socket manually to selected address */
        if (bind(peer->sd, (struct sockaddr *) &intf->if_addr, addrlen) < 0) {
            /* If we cannot bind to this address, set remaining entries
             * for this address from the reachable table to no connection
             * and try a new connection.
             */
            if ((EADDRINUSE == prte_socket_errno) || (EADDRNOTAVAIL == prte_socket_errno)) {
                for (j = 0; j < remote_if_count; j++) {
                    results->weights[best_i][j] = 0;
                }
                continue;
            }
            /* If we have another bind issue, something has gone horribly
             * wrong.
             */
            pmix_output(0, "%s bind() failed, can't recover : %s (%d)",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), strerror(prte_socket_errno),
                        prte_socket_errno);

            CLOSE_THE_SOCKET(peer->sd);
            tcp_peer_fail_queued_sends(peer, PRTE_ERR_UNREACH);
            PRTE_ACTIVATE_JOB_STATE(NULL, PRTE_JOB_STATE_COMM_FAILED);
            goto cleanup;
        }

    retry_connect:
        addr->retries++;

        rc = connect(peer->sd, (struct sockaddr *) &addr->addr, addrlen);
        if (rc < 0) {
            /* non-blocking so wait for completion */
            if (prte_socket_errno == EINPROGRESS || prte_socket_errno == EWOULDBLOCK) {
                pmix_output_verbose(
                    OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                    "%s waiting for connect completion to %s - activating send event",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name));
                /* just ensure the send_event is active */
                if (!peer->send_ev_active) {
                    prte_event_add(&peer->send_event, 0);
                    peer->send_ev_active = true;
                }
                PMIX_RELEASE(op);
                goto out;
            }

            /* Some kernels (Linux 2.6) will automatically software
             * abort a connection that was ECONNREFUSED on the last
             * attempt, without even trying to establish the
             * connection.  Handle that case in a semi-rational
             * way by trying twice before giving up
             */
            if (ECONNABORTED == prte_socket_errno) {
                if (addr->retries < prte_oob_base.max_retries) {
                    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT,
                                        prte_oob_base.output,
                                        "%s connection aborted by OS to %s - retrying",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                        PRTE_NAME_PRINT(&peer->name));
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
            peer->first_attempt = 0;
            break;
        }
    } // End of looping over reachable bitmap entries

    /* End of attempting connection */
    if (!connected) {
        /* it could be that the intended recipient just hasn't
         * started yet. if requested, wait awhile and try again
         * unless/until we hit the maximum number of retries */
        if (0 < prte_oob_base.retry_delay) {
            /* Bound how long we chase a peer that is not our lifeline (the
             * HNP/controller).  During a bootstrap race an interior parent may
             * never come up; rather than retry it forever we give up after
             * connect_max_time seconds and fall through to failed_to_connect,
             * which heals the routing tree up to the next ancestor.  The HNP
             * itself is always retried forever (per max_recon_attempts). */
            bool give_up_on_time = false;
            if (0 < prte_oob_base.connect_max_time
                && !PMIX_CHECK_PROCID(&peer->name, PRTE_PROC_MY_HNP)) {
                time_t now = time(NULL);
                if (0 == peer->first_attempt) {
                    peer->first_attempt = now;
                }
                if ((now - peer->first_attempt) >= (time_t) prte_oob_base.connect_max_time) {
                    give_up_on_time = true;
                }
            }
            if (!give_up_on_time
                && (prte_oob_base.max_recon_attempts < 0
                    || peer->num_retries < prte_oob_base.max_recon_attempts)) {
                struct timeval tv;
                /* close the current socket */
                CLOSE_THE_SOCKET(peer->sd);
                /* reset the addr states */
                PMIX_LIST_FOREACH(addr, &peer->addrs, prte_oob_tcp_addr_t)
                {
                    addr->state = MCA_OOB_TCP_UNCONNECTED;
                    addr->retries = 0;
                }
                /* give it awhile and try again.  The base case is a fixed
                 * delay of retry_delay seconds (unchanged behavior).  When
                 * retry_max_delay is larger, the delay backs off
                 * exponentially - retry_delay, 2x, 4x, ... - capped at
                 * retry_max_delay, so a daemon waiting on a not-yet-present
                 * peer polls frequently at first and then settles onto a
                 * steady rate rather than busy-spinning. */
                tv.tv_sec = prte_oob_base.retry_delay;
                if (prte_oob_base.retry_max_delay > prte_oob_base.retry_delay) {
                    uint64_t d;
                    /* guard the shift: num_retries grows without bound when we
                     * never give up, so clamp the exponent before shifting */
                    if (peer->num_retries >= 32) {
                        d = (uint64_t) prte_oob_base.retry_max_delay;
                    } else {
                        d = (uint64_t) prte_oob_base.retry_delay << peer->num_retries;
                        if (d > (uint64_t) prte_oob_base.retry_max_delay) {
                            d = (uint64_t) prte_oob_base.retry_max_delay;
                        }
                    }
                    tv.tv_sec = (time_t) d;
                }
                tv.tv_usec = 0;
                ++peer->num_retries;
                PRTE_RETRY_TCP_CONN_STATE(peer, prte_oob_tcp_peer_try_connect, &tv);
                goto cleanup;
            }
        }
        /* no address succeeded, so we cannot reach this peer */
        peer->state = MCA_OOB_TCP_FAILED;
        host = prte_get_proc_hostname(&(peer->name));
        if (NULL == host && NULL != peer->active_addr) {
            host = pmix_net_get_hostname((struct sockaddr *) &(peer->active_addr->addr));
        }
        /* Say which of the two failures this is, because they need opposite
         * advice and only one of them is about the network.
         *
         * A peer we have NEVER reached may not have started yet, may have
         * failed to start, or may be behind a firewall - the connection has
         * never worked, so the configuration is a fair thing to suspect.
         *
         * A peer we HAVE reached is a different story: the connection worked,
         * so the network and the firewall are exonerated by that fact alone.
         * The daemon has gone away since. Telling that user to check iptables
         * sends them at their network when the answer is almost always their
         * node or their scheduler - a cancelled or expired allocation takes
         * its daemons with it, and that is the common case on a managed
         * cluster. (The errmgr's own "lost communication" report says this
         * properly, but show_help aggregates by topic and this message
         * usually gets there first, so this is the one the user reads.)
         *
         * pmix_output rather than show_help throughout: we may well not be
         * connected to the HNP at this point. */
        if (peer->ever_connected) {
            pmix_output(prte_clean_output,
                        "------------------------------------------------------------\n"
                        "A daemon that was running is no longer reachable:\n"
                        "  Local host:    %s\n"
                        "  Remote host:   %s\n"
                        "The connection to that daemon was working earlier, so this is\n"
                        "not a firewall or network configuration problem. Something\n"
                        "ended the daemon or the node it was on. The usual causes are\n"
                        "that the node failed, that the daemon was killed, or that the\n"
                        "resource manager reclaimed the allocation it was running in -\n"
                        "a job that was cancelled, or that reached its time limit.\n"
                        "------------------------------------------------------------",
                        prte_process_info.nodename, (NULL == host) ? "<unknown>" : host);
        } else {
            pmix_output(prte_clean_output,
                        "------------------------------------------------------------\n"
                        "A daemon was unable to complete a TCP connection\n"
                        "to another daemon:\n"
                        "  Local host:    %s\n"
                        "  Remote host:   %s\n"
                        "This connection has never succeeded, so the daemon may have\n"
                        "failed to start, or may not be reachable. This is usually\n"
                        "caused by a firewall on the remote host. Please check that any\n"
                        "firewall (e.g., iptables) has been disabled and try again.\n"
                        "------------------------------------------------------------",
                        prte_process_info.nodename, (NULL == host) ? "<unknown>" : host);
        }
        /* close the socket */
        CLOSE_THE_SOCKET(peer->sd);
        /* let the TCP component know that we failed to make the connection
         * so it can do its bookkeeping - this fires asynchronously, on
         * prte_event_base */
        PRTE_ACTIVATE_TCP_CMP_OP(peer, prte_mca_oob_tcp_component_failed_to_connect);
        tcp_peer_fail_queued_sends(peer, PRTE_ERR_UNREACH);
        goto cleanup;
    }

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s prte_tcp_peer_try_connect: "
                        "Connection to proc %s succeeded",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name));

    /* setup our recv to catch the return ack call */
    if (!peer->recv_ev_active) {
        prte_event_add(&peer->recv_event, 0);
        peer->recv_ev_active = true;
    }

    /* send our globally unique process identifier to the peer */
    if (PRTE_SUCCESS == (rc = tcp_peer_send_connect_ack(peer))) {
        peer->state = MCA_OOB_TCP_CONNECT_ACK;
    } else {
        /* The connection came up and died before it would take a few hundred
         * bytes, so this address has failed.  peer_close is what deals with
         * that: it drops the read event armed above with the socket, moves on
         * to the peer's next address with the queued messages intact, and
         * reports the peer only when no address is left.  This used to be
         * read as the simultaneous-connect race and dialed again on the spot
         * - but the send had already closed the peer, which had already
         * dialed again, so two attempts ran at once and the second closed the
         * first's socket out from under its armed events. */
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s prte_tcp_peer_try_connect: "
                            "tcp_peer_send_connect_ack to proc %s on %s:%d failed: %s (%d)",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                            pmix_net_get_hostname((struct sockaddr *) &addr->addr),
                            pmix_net_get_port((struct sockaddr *) &addr->addr), prte_strerror(rc), rc);
        peer->state = MCA_OOB_TCP_FAILED;
        prte_oob_tcp_peer_close(peer);
    }

cleanup:
    PMIX_RELEASE(op);
out:
    if (NULL != results) {
        /* a reference-counted object whose destructor frees the single block
         * backing the whole weight matrix - free()ing the object itself leaks
         * that block on every connection attempt */
        PMIX_RELEASE(results);
    }
    if (NULL != remote_list) {
        /* the list destructor does not touch the items, and every pmix_pif_t
         * on this list was allocated above just for this call */
        PMIX_LIST_RELEASE(remote_list);
    }
}

/* send a handshake that includes our process identifier and our version
 * string, so the peer can check it is talking to a daemon of its own DVM
 * running its own build
 */
static int tcp_peer_send_connect_ack(prte_oob_tcp_peer_t *peer)
{
    char *msg;
    prte_oob_tcp_hdr_t hdr;
    uint16_t ack_flag = htons(1);
    size_t sdsize, hdrsize, offset = 0;

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s SEND CONNECT ACK", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* load the header. Zero it first: the whole struct goes on the wire, so
     * every field - including the epoch and any padding - has to be defined */
    memset(&hdr, 0, sizeof(hdr));
    hdr.origin = PRTE_PROC_MY_NAME->rank;
    hdr.dst = peer->name.rank;
    /* the header carries one nspace and the receiver reads it as the
     * origin's, which is the only one this handshake is asked about */
    PRTE_OOB_TCP_HDR_LOAD_NSPACE(&hdr, PRTE_PROC_MY_NAME->nspace);
    hdr.type = MCA_OOB_TCP_IDENT;
    hdr.tag = 0;
    hdr.seq_num = 0;
    hdr.epoch = prte_rml_boot_epoch;

    /* payload size */
    sdsize = sizeof(ack_flag) + strlen(prte_version_string) + 1;
    hdr.nbytes = sdsize;
    hdrsize = PRTE_OOB_TCP_HDR_LEN(&hdr);
    MCA_OOB_TCP_HDR_HTON(&hdr);

    /* create a space for our message */
    sdsize += hdrsize;
    if (NULL == (msg = (char *) malloc(sdsize))) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the message - only the used part of the header goes on the wire */
    memcpy(msg + offset, &hdr, hdrsize);
    offset += hdrsize;
    memcpy(msg + offset, &ack_flag, sizeof(ack_flag));
    offset += sizeof(ack_flag);
    memcpy(msg + offset, prte_version_string, strlen(prte_version_string) + 1);
    offset += strlen(prte_version_string) + 1;

    /* send it.  A failure is the caller's to act on, and every caller closes
     * the peer for it - closing here as well made that two closes, and each
     * close of a connection still coming up starts a fresh attempt */
    if (PRTE_SUCCESS != tcp_peer_send_blocking(peer->sd, msg, sdsize)) {
        free(msg);
        return PRTE_ERR_UNREACH;
    }
    free(msg);

    return PRTE_SUCCESS;
}

/* refuse a handshake: our process identifier and an ack flag of zero, which
 * tells a peer that dialed us while we were dialing it that ours is the
 * connection that will be kept
 */
static int tcp_peer_send_connect_nack(int sd, pmix_proc_t *name)
{
    char *msg;
    prte_oob_tcp_hdr_t hdr;
    uint16_t ack_flag = htons(0);
    int rc = PRTE_SUCCESS;
    size_t sdsize, hdrsize, offset = 0;

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s SEND CONNECT NACK", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* load the header - see the note in tcp_peer_send_connect_ack */
    memset(&hdr, 0, sizeof(hdr));
    hdr.origin = PRTE_PROC_MY_NAME->rank;
    hdr.dst = name->rank;
    PRTE_OOB_TCP_HDR_LOAD_NSPACE(&hdr, PRTE_PROC_MY_NAME->nspace);
    hdr.type = MCA_OOB_TCP_IDENT;
    hdr.tag = 0;
    hdr.seq_num = 0;
    hdr.epoch = prte_rml_boot_epoch;

    /* payload size */
    sdsize = sizeof(ack_flag);
    hdr.nbytes = sdsize;
    hdrsize = PRTE_OOB_TCP_HDR_LEN(&hdr);
    MCA_OOB_TCP_HDR_HTON(&hdr);

    /* create a space for our message */
    sdsize += hdrsize;
    if (NULL == (msg = (char *) malloc(sdsize))) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the message - only the used part of the header goes on the wire */
    memcpy(msg + offset, &hdr, hdrsize);
    offset += hdrsize;
    memcpy(msg + offset, &ack_flag, sizeof(ack_flag));
    offset += sizeof(ack_flag);

    /* send it */
    if (PRTE_SUCCESS != tcp_peer_send_blocking(sd, msg, sdsize)) {
        /* it's ok if it fails - remote side may already
         * have identified the collision and closed the connection
         */
        rc = PRTE_SUCCESS;
    }
    free(msg);
    return rc;
}

/*
 * Initialize events to be used by the peer instance for TCP select/poll callbacks.
 *
 * These start out on prte_event_base whatever base the peer was assigned:
 * connect completion and the IDENT handshake run the connection state machine,
 * which owns the peer table and the routing tree and therefore belongs to the
 * main progress thread.  Once the handshake succeeds, tcp_peer_connected()
 * moves both events onto peer->evbase for the life of the connection.
 */
static void tcp_peer_event_init(prte_oob_tcp_peer_t *peer)
{
    if (peer->sd >= 0) {
        assert(!peer->send_ev_active && !peer->recv_ev_active);
        prte_event_set(prte_event_base, &peer->recv_event, peer->sd, PRTE_EV_READ | PRTE_EV_PERSIST,
                       prte_oob_tcp_recv_handler, peer);
        if (peer->recv_ev_active) {
            prte_event_del(&peer->recv_event);
            peer->recv_ev_active = false;
        }

        prte_event_set(prte_event_base, &peer->send_event, peer->sd,
                       PRTE_EV_WRITE | PRTE_EV_PERSIST, prte_oob_tcp_send_handler, peer);
        if (peer->send_ev_active) {
            prte_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }
    }
}

/*
 * Hand this peer's socket over to the base that will service it for the rest
 * of the connection's life.
 *
 * Called from tcp_peer_connected() - i.e. on the main progress thread, at the
 * one moment when the handshake is finished and no data event has yet fired.
 * Both events are deleted first: libevent will not re-target a pending event,
 * and deleting the event whose callback we are inside (the recv event, on the
 * connect-ack path) is explicitly allowed.  The caller re-adds whichever
 * events it wants live, which is why the active flags are cleared here.
 *
 * When no OOB worker threads were requested peer->evbase IS prte_event_base
 * and this is a delete/re-set onto the base the events were already on - a few
 * instructions once per connection, and nothing else changes.
 */
static void tcp_peer_rebind_events(prte_oob_tcp_peer_t *peer)
{
    if (0 > peer->sd) {
        return;
    }
    if (peer->recv_ev_active) {
        prte_event_del(&peer->recv_event);
        peer->recv_ev_active = false;
    }
    if (peer->send_ev_active) {
        prte_event_del(&peer->send_event);
        peer->send_ev_active = false;
    }
    prte_event_set(peer->evbase, &peer->recv_event, peer->sd, PRTE_EV_READ | PRTE_EV_PERSIST,
                   prte_oob_tcp_recv_handler, peer);
    prte_event_set(peer->evbase, &peer->send_event, peer->sd, PRTE_EV_WRITE | PRTE_EV_PERSIST,
                   prte_oob_tcp_send_handler, peer);
}

/*
 * Start the connection state machine for a peer that a socket-servicing thread
 * found unconnected.
 *
 * The state word, the address list and the retry bookkeeping all belong to the
 * main progress thread, so a worker that has a message to send but no live
 * connection posts here rather than driving the machine itself.  Runs on
 * prte_event_base and consumes the conn op it was handed.
 */
void prte_oob_tcp_peer_start_connect(int fd, short args, void *cbdata)
{
    prte_oob_tcp_conn_op_t *op = (prte_oob_tcp_conn_op_t *) cbdata;
    prte_oob_tcp_peer_t *peer;

    PMIX_ACQUIRE_OBJECT(op);
    peer = op->peer;

    if (MCA_OOB_TCP_CONNECTING == peer->state || MCA_OOB_TCP_CONNECT_ACK == peer->state) {
        /* somebody beat us to it */
        PMIX_RELEASE(op);
        return;
    }
    if (MCA_OOB_TCP_CONNECTED == peer->state) {
        /* it came up while we were in flight - just make sure the queued
         * message will actually go out.  Look again under the lock: a
         * connection being torn down still reads CONNECTED until its
         * teardown, which holds the lock, is complete */
        pmix_mutex_lock(&peer->lock);
        if (MCA_OOB_TCP_CONNECTED == peer->state) {
            if (NULL == peer->send_msg) {
                peer->send_msg = (prte_oob_tcp_send_t *) pmix_list_remove_first(&peer->send_queue);
            }
            if (NULL != peer->send_msg && !peer->send_ev_active) {
                peer->send_ev_active = true;
                PMIX_POST_OBJECT(peer);
                prte_event_add(&peer->send_event, 0);
            }
            pmix_mutex_unlock(&peer->lock);
            PMIX_RELEASE(op);
            return;
        }
        pmix_mutex_unlock(&peer->lock);
    }

    peer->state = MCA_OOB_TCP_CONNECTING;
    /* hand the op straight on - try_connect owns it from here */
    prte_oob_tcp_peer_try_connect(fd, args, op);
}

/*
 * Check the status of the connection. If the connection failed, will retry
 * later. Otherwise, send this processes identifier to the peer on the
 * newly connected socket.
 */
void prte_oob_tcp_peer_complete_connect(prte_oob_tcp_peer_t *peer)
{
    int so_error = 0;
    prte_socklen_t so_length = sizeof(so_error);

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s:tcp:complete_connect called for peer %s on socket %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name), peer->sd);

    /* check connect completion status */
    if (getsockopt(peer->sd, SOL_SOCKET, SO_ERROR, (char *) &so_error, &so_length) < 0) {
        pmix_output(0, "%s tcp_peer_complete_connect: getsockopt() to %s failed: %s (%d)\n",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                    strerror(prte_socket_errno), prte_socket_errno);
        peer->state = MCA_OOB_TCP_FAILED;
        prte_oob_tcp_peer_close(peer);
        return;
    }

    if (so_error == EINPROGRESS) {
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s:tcp:send:handler still in progress",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        return;
    } else if (so_error == ECONNREFUSED || so_error == ETIMEDOUT) {
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s-%s tcp_peer_complete_connect: connection failed: %s (%d)",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                            strerror(so_error), so_error);
        prte_oob_tcp_peer_close(peer);
        return;
    } else if (so_error != 0) {
        /* No need to worry about the return code here - we return regardless
           at this point, and if an error did occur a message has already been
           printed for the user */
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s-%s tcp_peer_complete_connect: "
                            "connection failed with error %d",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                            so_error);
        prte_oob_tcp_peer_close(peer);
        return;
    }

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s tcp_peer_complete_connect: "
                        "sending ack to %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)));

    if (tcp_peer_send_connect_ack(peer) == PRTE_SUCCESS) {
        peer->state = MCA_OOB_TCP_CONNECT_ACK;
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s tcp_peer_complete_connect: "
                            "setting read event on connection to %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)));

        if (!peer->recv_ev_active) {
            peer->recv_ev_active = true;
            PMIX_POST_OBJECT(peer);
            prte_event_add(&peer->recv_event, 0);
        }
    } else {
        pmix_output(0, "%s tcp_peer_complete_connect: unable to send connect ack to %s",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)));
        peer->state = MCA_OOB_TCP_FAILED;
        prte_oob_tcp_peer_close(peer);
    }
}

/*
 * A blocking send on a non-blocking socket. Used to send the small amount of connection
 * information that identifies the peers endpoint.
 */
static int tcp_peer_send_blocking(int sd, void *data, size_t size)
{
    unsigned char *ptr = (unsigned char *) data;
    size_t cnt = 0;
    int retval;

    PMIX_ACQUIRE_OBJECT(ptr);

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s send blocking of %" PRIsize_t " bytes to socket %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), size, sd);

    while (cnt < size) {
        retval = send(sd, (char *) ptr + cnt, size - cnt, 0);
        if (retval < 0) {
            if (prte_socket_errno != EINTR && prte_socket_errno != EAGAIN
                && prte_socket_errno != EWOULDBLOCK) {
                pmix_output(0, "%s tcp_peer_send_blocking: send() to socket %d failed: %s (%d)\n",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), sd, strerror(prte_socket_errno),
                            prte_socket_errno);
                return PRTE_ERR_UNREACH;
            }
            continue;
        }
        cnt += retval;
    }

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s blocking send complete to socket %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), sd);

    return PRTE_SUCCESS;
}

/*
 *  Receive the peers globally unique process identification from a newly
 *  connected socket and verify the expected response. If so, move the
 *  socket to a connected state.
 */
static bool retry(prte_oob_tcp_peer_t *peer, int sd)
{
    int cmpval;

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s SIMUL CONNECTION WITH %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&peer->name));
    cmpval = prte_util_compare_name_fields(PRTE_NS_CMP_ALL, &peer->name, PRTE_PROC_MY_NAME);
    if (PRTE_VALUE1_GREATER == cmpval) {
        /* The other end will retry the connection.
         *
         * What we abandon may be a working connection, whose events live
         * on a worker base and are armed by prte_oob_tcp_queue_msg under
         * the peer lock whenever the peer reads CONNECTED.  So leave
         * CONNECTED under that lock first - after it, nothing re-arms the
         * send event behind the deletes below, and the deletes then wait
         * out any handler already running (including one closing the
         * peer, whose own teardown this state keeps quiet). */
        pmix_mutex_lock(&peer->lock);
        peer->state = MCA_OOB_TCP_UNCONNECTED;
        pmix_mutex_unlock(&peer->lock);
        if (peer->send_ev_active) {
            prte_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }
        if (peer->recv_ev_active) {
            prte_event_del(&peer->recv_event);
            peer->recv_ev_active = false;
        }
        CLOSE_THE_SOCKET(peer->sd);
        /* We have just thrown away our own socket in favour of the one
         * the caller accepted, and the caller carries on to finish the
         * handshake there, so that socket is the peer's from here on.
         * Say so: leaving the descriptor we closed in the field invites a
         * second close of a number the kernel has since handed to
         * somebody else, and leaves anything that fails later in the
         * handshake unable to tell that this peer owns the socket it
         * failed on. */
        peer->sd = sd;
        /* Nothing part way through the old stream means anything on the
         * new one - the far end threw it away with its end of that
         * connection.  A half-read message would have the new stream's
         * bytes appended to it, and a half-sent one would carry on from
         * the middle; drop the first, and start the second again from its
         * header. */
        if (NULL != peer->recv_msg) {
            PMIX_RELEASE(peer->recv_msg);
            peer->recv_msg = NULL;
        }
        pmix_mutex_lock(&peer->lock);
        if (NULL != peer->send_msg) {
            peer->send_msg->hdr_sent = false;
            peer->send_msg->iovnum = 0;
            peer->send_msg->sdptr = (char *) &peer->send_msg->hdr;
            peer->send_msg->sdbytes = PRTE_OOB_TCP_HDR_LEN(&peer->send_msg->hdr);
        }
        peer->state = MCA_OOB_TCP_UNCONNECTED;
        pmix_mutex_unlock(&peer->lock);
        return false;
    } else {
        /* The connection will be retried */
        tcp_peer_send_connect_nack(sd, &peer->name);
        CLOSE_THE_SOCKET(sd);
        return true;
    }
}

int prte_oob_tcp_peer_recv_connect_ack(prte_oob_tcp_peer_t *pr, int sd,
                                       prte_oob_tcp_handshake_t *hs, prte_oob_tcp_hdr_t *dhdr)
{
    char *msg;
    char *version;
    size_t offset = 0;
    prte_oob_tcp_hdr_t hdr;
    prte_oob_tcp_peer_t *peer;
    pmix_proc_t sender;
    uint16_t ack_flag;
    bool is_new = (NULL == pr);
    int rc;

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s RECV CONNECT ACK FROM %s ON SOCKET %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == pr) ? "UNKNOWN" : PRTE_NAME_PRINT(&pr->name), sd);

    peer = pr;
    /* a reply is only expected on a connection we dialed and are waiting on */
    if (NULL != peer && MCA_OOB_TCP_CONNECT_ACK != peer->state) {
        /* handshake broke down - abort this connection */
        pmix_output(0, "%s RECV CONNECT BAD HANDSHAKE (%d) FROM %s ON SOCKET %d",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), peer->state,
                    PRTE_NAME_PRINT(&(peer->name)), sd);
        prte_oob_tcp_peer_close(peer);
        return PRTE_ERR_UNREACH;
    }

    rc = tcp_peer_read_handshake(sd, hs);
    if (PRTE_ERR_WOULD_BLOCK == rc) {
        /* the rest is still on its way */
        return rc;
    }
    if (PRTE_SUCCESS != rc) {
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s unable to complete recv of connect-ack from %s ON SOCKET %d: %s",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            (NULL == peer) ? "UNKNOWN" : PRTE_NAME_PRINT(&peer->name), sd,
                            prte_strerror(rc));
        prte_oob_tcp_handshake_reset(hs);
        if (NULL != peer) {
            peer->state = MCA_OOB_TCP_FAILED;
            prte_oob_tcp_peer_close(peer);
        } else {
            CLOSE_THE_SOCKET(sd);
        }
        return rc;
    }

    /* the whole handshake is in, and its header has been checked and
     * converted - take it, and leave the record empty for any next one */
    hdr = hs->hdr;
    msg = hs->payload;
    hs->payload = NULL;
    prte_oob_tcp_handshake_reset(hs);

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s connect-ack recvd from %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (NULL == peer) ? "UNKNOWN" : PRTE_NAME_PRINT(&peer->name));

    /* rebuild the sender's identity, which the header carries as a rank
     * plus the nspace read above */
    PRTE_OOB_TCP_HDR_PROC(&hdr, hdr.origin, &sender);
    /* if the requestor wanted the header returned, then do so now */
    if (NULL != dhdr) {
        *dhdr = hdr;
    }

    if (MCA_OOB_TCP_PROBE == hdr.type) {
        size_t hdrsize;
        if (NULL != peer) {
            /* a probe opens a conversation; it is no answer to one we
             * opened, and replying on - then closing - the socket the peer
             * owns would leave the peer holding a dead descriptor that it
             * goes on to treat as connected */
            pmix_output(0, "%s tcp_peer_recv_connect_ack: probe received in reply to a "
                        "connection to %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&(peer->name)));
            peer->state = MCA_OOB_TCP_FAILED;
            prte_oob_tcp_peer_close(peer);
            return PRTE_ERR_COMM_FAILURE;
        }
        /* send a header back */
        hdr.type = MCA_OOB_TCP_PROBE;
        hdr.dst = hdr.origin;
        hdr.origin = PRTE_PROC_MY_NAME->rank;
        PRTE_OOB_TCP_HDR_LOAD_NSPACE(&hdr, PRTE_PROC_MY_NAME->nspace);
        hdrsize = PRTE_OOB_TCP_HDR_LEN(&hdr);
        MCA_OOB_TCP_HDR_HTON(&hdr);
        tcp_peer_send_blocking(sd, &hdr, hdrsize);
        CLOSE_THE_SOCKET(sd);
        return PRTE_SUCCESS;
    }

    /* if we don't already have it, get the peer */
    if (NULL == peer) {
        peer = prte_oob_tcp_peer_lookup(&sender);
        if (NULL == peer) {
            pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                "%s prte_oob_tcp_recv_connect: connection from new peer",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
            peer = PMIX_NEW(prte_oob_tcp_peer_t);
            PMIX_XFER_PROCID(&peer->name, &sender);
            peer->state = MCA_OOB_TCP_ACCEPTING;
            pmix_list_append(&prte_oob_base.peers, &peer->super);
        }
    } else {
        /* compare the peers name to the expected value */
        if (!PMIX_CHECK_PROCID(&peer->name, &sender)) {
            pmix_output(0,
                        "%s tcp_peer_recv_connect_ack: "
                        "received unexpected process identifier %s from %s\n",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&sender),
                        PRTE_NAME_PRINT(&(peer->name)));
            free(msg);
            peer->state = MCA_OOB_TCP_FAILED;
            prte_oob_tcp_peer_close(peer);
            return PRTE_ERR_CONNECTION_REFUSED;
        }
    }

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s connect-ack header from %s is okay", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&peer->name));

    /* Check the type of acknowledgement */
    memcpy(&ack_flag, msg + offset, sizeof(ack_flag));
    offset += sizeof(ack_flag);

    ack_flag = ntohs(ack_flag);
    if (!ack_flag) {
        free(msg);
        if (is_new) {
            /* A nack says "you dialed me while I was dialing you, and I am
             * the one who will finish it".  That is an answer to a call we
             * placed, so it belongs on a socket we opened; arriving on one
             * somebody just opened to us it says nothing about any connection
             * of ours.  Drop the socket and leave the peer - and whatever
             * attempt it may have in flight - untouched. */
            CLOSE_THE_SOCKET(sd);
            return PRTE_ERR_UNREACH;
        }

        /* We got a nack on our own connection attempt, so the remote side
         * will be the initiator.  The state check at the top of this function
         * has already established that we are in CONNECT_ACK - a peer in any
         * other state never gets this far.
         */

        /* unregister active events - before the socket they watch is gone */
        if (peer->recv_ev_active) {
            prte_event_del(&peer->recv_event);
            peer->recv_ev_active = false;
        }
        if (peer->send_ev_active) {
            prte_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }

        /* release the socket */
        CLOSE_THE_SOCKET(peer->sd);

        /* change the state so we'll accept the remote
         * connection when it appears
         */
        peer->state = MCA_OOB_TCP_UNCONNECTED;
        return PRTE_ERR_UNREACH;
    }

    /* check for a race condition - if I was in the process of
     * creating a connection to the peer, or have already established
     * such a connection, then we need to reject this connection. We will
     * let the higher ranked process retry - if I'm the lower ranked
     * process, I'll simply defer until I receive the request
     */
    if (is_new
        && (MCA_OOB_TCP_CONNECTED == peer->state || MCA_OOB_TCP_CONNECTING == peer->state
            || MCA_OOB_TCP_CONNECT_ACK == peer->state)) {
        if (retry(peer, sd)) {
            free(msg);
            return PRTE_ERR_UNREACH;
        }
    }

    if (hdr.nbytes == offset) {
        // missing version string
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-oob-tcp.txt", "missing version", true,
                       prte_process_info.nodename, PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                       pmix_fd_get_peer_name(sd), PRTE_NAME_PRINT(&(peer->name)));
        abort_handshake(peer, sd);
        free(msg);
        return PRTE_ERR_CONNECTION_REFUSED;
    }

    /* check that this is from a matching version.  The reader terminated the
     * payload, so this is a string even if the peer sent none */
    version = (char *) ((char *) msg + offset);
    if (0 != strcmp(version, prte_version_string)) {
        prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-oob-tcp.txt", "version mismatch", true, prte_process_info.nodename,
                       PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), prte_version_string,
                       pmix_fd_get_peer_name(sd), PRTE_NAME_PRINT(&(peer->name)), version);

        abort_handshake(peer, sd);
        free(msg);
        return PRTE_ERR_CONNECTION_REFUSED;
    }
    free(msg);

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s connect-ack version from %s matches ours",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name));

    /* if the requestor wanted the header returned, then they
     * will complete their processing
     */
    if (NULL != dhdr) {
        return PRTE_SUCCESS;
    }

    /* connected */
    tcp_peer_connected(peer);
    if (OOB_TCP_DEBUG_CONNECT
        <= pmix_output_get_verbosity(prte_oob_base.output)) {
        prte_oob_tcp_peer_dump(peer, "connected");
    }
    return PRTE_SUCCESS;
}

/*
 *  Setup peer state to reflect that connection has been established,
 *  and start any pending sends.
 */
static void tcp_peer_connected(prte_oob_tcp_peer_t *peer)
{
    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s-%s tcp_peer_connected on socket %d", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&(peer->name)), peer->sd);

    if (NULL != peer->active_addr) {
        peer->active_addr->retries = 0;
    }

    /* Publishing CONNECTED is what makes this peer reachable from a worker
     * base: prte_oob_base_send_nb tests the state and MCA_OOB_TCP_QUEUE_MSG
     * then posts to peer->evbase, where prte_oob_tcp_queue_msg adds the send
     * event.  So the events must already BE on that base before the state is
     * published, and nothing may be re-targeting them while a worker adds
     * one - libevent's event_assign on an event another thread is adding is
     * undefined, and what it produced here was an event registered on nothing:
     * the send never fired and every thread in the DVM parked in epoll_wait.
     *
     * Hence: rebind, publish, and start the pending sends as one step under
     * the peer lock.  Taking the lock across the rebind is safe precisely
     * because the events are still on prte_event_base at this point and we
     * are on it, so those event_del calls cannot block waiting for another
     * thread's callback (which is the deadlock peer_close has to avoid). */
    pmix_mutex_lock(&peer->lock);
    tcp_peer_rebind_events(peer);
    peer->state = MCA_OOB_TCP_CONNECTED;
    peer->established = true;
    peer->ever_connected = true;

    /* start listening on the connection - here, under the lock, and not by
     * the caller afterwards: from this point a worker may already be failing
     * a send on the socket, and a teardown that ran before the read event was
     * marked active would leave it armed on the dead descriptor */
    if (!peer->recv_ev_active) {
        peer->recv_ev_active = true;
        PMIX_POST_OBJECT(peer);
        prte_event_add(&peer->recv_event, 0);
    }

    /* initiate send of first message on queue */
    if (NULL == peer->send_msg) {
        peer->send_msg = (prte_oob_tcp_send_t *) pmix_list_remove_first(&peer->send_queue);
    }
    if (NULL != peer->send_msg && !peer->send_ev_active) {
        peer->send_ev_active = true;
        PMIX_POST_OBJECT(peer);
        prte_event_add(&peer->send_event, 0);
    }
    pmix_mutex_unlock(&peer->lock);
}

/*
 * Remove any event registrations associated with the socket
 * and update the peer state to reflect the connection has
 * been closed.
 */
void prte_oob_tcp_peer_close(prte_oob_tcp_peer_t *peer)
{
    prte_oob_tcp_state_t old_state;

    /* Take the state transition under the peer lock so exactly one caller
     * performs the teardown.  Reachable from both the main progress thread
     * (the connection state machine) and, once the socket has been handed to
     * a worker base, from that worker's send/recv handler - and an unguarded
     * read-then-write of the state word lets both through. */
    pmix_mutex_lock(&peer->lock);
    if (MCA_OOB_TCP_CLOSED == peer->state) {
        pmix_mutex_unlock(&peer->lock);
        return;
    }

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s tcp_peer_close for %s sd %d state %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                        peer->sd, prte_oob_tcp_state_print(peer->state));

    /* If this connection was still being established, mark the address as
     * failed and cycle back to try the next one.  A peer is handed a list of
     * addresses so that a bad one is not fatal, and an attempt can die three
     * ways before the connection is usable: connect() itself failed
     * (CONNECTING), something answered but the IDENT handshake did not
     * complete (CONNECT_ACK), or a caller set FAILED on the way in.  Only
     * CONNECTING used to rotate, so a peer that failed the handshake was
     * closed with its remaining addresses untried and its queued messages
     * dropped, reporting nothing to anyone.  With no follow-up traffic to
     * trigger a fresh connect, as with a daemon's first report to the HNP,
     * the DVM never finished coming up and the job hung.
     *
     * Rotating above the queued-send flush below is deliberate: the messages
     * waiting on this peer must survive to go out over the next address.  The
     * established flag keeps the paths that close a *working* connection out
     * of this branch, so those still report a lost connection instead of
     * silently reconnecting.  It also guarantees the socket events are still
     * on prte_event_base, since they move to peer->evbase only at CONNECTED,
     * so deleting them here cannot block on a worker's callback. */
    if (!peer->established
        && (MCA_OOB_TCP_CONNECTING == peer->state || MCA_OOB_TCP_CONNECT_ACK == peer->state
            || MCA_OOB_TCP_FAILED == peer->state)) {
        /* try_connect never sets the peer state; every caller does it first,
         * so leaving CLOSED here would advertise a dead peer while a
         * connection attempt was in flight */
        peer->state = MCA_OOB_TCP_CONNECTING;
        pmix_mutex_unlock(&peer->lock);
        /* drop any registration on the socket we are about to discard:
         * tcp_peer_event_init asserts that neither event is active, and
         * try_connect always reaches it once peer->sd is reset below */
        if (peer->recv_ev_active) {
            prte_event_del(&peer->recv_event);
            peer->recv_ev_active = false;
        }
        if (peer->send_ev_active) {
            prte_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }
        close(peer->sd);
        peer->sd = -1;
        if (NULL != peer->active_addr) {
            /* FAILED stops try_connect from picking this address again, so
             * the candidate set shrinks and the rotation terminates.  With
             * nothing untried left, try_connect reports the failure and
             * activates failed_to_connect, so we never give up silently. */
            peer->active_addr->state = MCA_OOB_TCP_FAILED;
        }
        PRTE_ACTIVATE_TCP_CONN_STATE(peer, prte_oob_tcp_peer_try_connect);
        return;
    }

    old_state = peer->state;
    peer->established = false;

    /* Take the connection apart BEFORE saying it is closed, and under the
     * lock.  CLOSED is what the main progress thread acts on - the next
     * message for this peer starts a new connection - and a new connection
     * reuses peer->sd and both event structures.  Published first, the main
     * thread could be setting those events up again while this thread was
     * still deleting them, and this thread's close could land on the
     * descriptor the new attempt had just been given.  A worker preempted in
     * that window is all it takes, and on a node busy with application
     * processes that is not rare.
     *
     * Holding the lock across the deletes is safe here, where it would not be
     * in the branch above: a connection that reached CONNECTED is only ever
     * closed by the thread servicing its socket, which is where its events
     * live, so there is no other thread's handler for the deletes to wait on.
     * A thread wanting the lock meanwhile waits for the teardown to finish;
     * one deleting these events itself (retry()) is already waiting for the
     * handler this is running inside to return. */
    if (NULL != peer->active_addr) {
        peer->active_addr->state = MCA_OOB_TCP_CLOSED;
    }
    if (peer->recv_ev_active) {
        prte_event_del(&peer->recv_event);
        peer->recv_ev_active = false;
    }
    if (peer->send_ev_active) {
        prte_event_del(&peer->send_event);
        peer->send_ev_active = false;
    }
    close(peer->sd);
    peer->sd = -1;

    /* clean up any partial recv - the recv object's destructor disposes of
     * whatever payload had already been read into it */
    if (NULL != peer->recv_msg) {
        PMIX_RELEASE(peer->recv_msg);
        peer->recv_msg = NULL;
    }
    peer->state = MCA_OOB_TCP_CLOSED;
    pmix_mutex_unlock(&peer->lock);

    /* inform rml of all queued sends' completion (as failures)
     * do not try to re-queue messages at this level - risking message loss is
     * unavoidable when a node in the communication tree dies, so safely
     * replaying messages must be handled at a higher level.
     */
    tcp_peer_fail_lost_sends(peer);

    /* inform the component-level that we have lost a connection so
     * it can decide what to do about it.
     */
    if (MCA_OOB_TCP_CONNECTED == old_state) {
        /* ...unless we have been told to pretend we did not notice this one.
         * Reporting the loss is what gets the node marked down, and a node
         * marked down turns every later message for it into an immediate
         * refusal - which is the whole reason the connect-failure path is so
         * hard to reach on purpose.  Staying quiet leaves this process still
         * believing in the daemon, so the next message for it opens a fresh
         * connection and fails there instead.  See prte_oob_silent_loss_vpid. */
        if (0 <= prte_oob_base.silent_loss_vpid
            && (pmix_rank_t) prte_oob_base.silent_loss_vpid == peer->name.rank) {
            pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                "%s tcp_peer_close: NOT reporting loss of %s (fault injection)",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                PRTE_NAME_PRINT(&(peer->name)));
        } else {
            PRTE_ACTIVATE_TCP_CMP_OP(peer, prte_mca_oob_tcp_component_lost_connection);
        }
    }
}

void prte_oob_tcp_handshake_reset(prte_oob_tcp_handshake_t *hs)
{
    if (NULL != hs->payload) {
        free(hs->payload);
    }
    memset(hs, 0, sizeof(*hs));
}

/*
 * Take what the socket has of `want` bytes, adding to the `*have` already
 * read.  PRTE_SUCCESS once all of them are in, PRTE_ERR_WOULD_BLOCK when the
 * socket runs dry first.
 */
static int tcp_peer_read_some(int sd, char *dst, size_t want, size_t *have)
{
    ssize_t retval;

    while (*have < want) {
        retval = recv(sd, dst + *have, want - *have, 0);
        if (0 < retval) {
            *have += (size_t) retval;
            continue;
        }
        if (0 == retval) {
            pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                "%s tcp_peer_read_some: peer closed connection on socket %d",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), sd);
            return PRTE_ERR_UNREACH;
        }
        if (EINTR == prte_socket_errno) {
            continue;
        }
        if (EAGAIN == prte_socket_errno || EWOULDBLOCK == prte_socket_errno) {
            return PRTE_ERR_WOULD_BLOCK;
        }
        /* includes the reset a peer whose listen backlog overflowed sends
         * after the three-way handshake: that attempt has failed, and
         * closing it moves the peer on to try again */
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s tcp_peer_read_some: recv() on socket %d failed: %s (%d)",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), sd,
                            strerror(prte_socket_errno), prte_socket_errno);
        return PRTE_ERR_COMM_FAILURE;
    }
    return PRTE_SUCCESS;
}

/*
 * Read as much of a connect handshake as has arrived on `sd` into `hs`.
 *
 * This runs on the progress thread, so it never waits: PRTE_ERR_WOULD_BLOCK
 * says the socket ran dry first, and the caller comes back on its next read
 * event.  The listening port accepts anyone, and a handshake read by waiting
 * for its bytes let one stray byte on it park the progress thread for as long
 * as its sender cared to stay connected - blocked in recv() on Linux, where
 * an accepted socket starts out blocking, and spinning on EAGAIN elsewhere.
 * Nothing else in the daemon ran meanwhile.
 *
 * The header is checked as soon as it is complete, before any payload is
 * allocated: a connection that does not belong to this DVM, or does not
 * speak this protocol, is turned away on what it has already sent rather than
 * being allowed to name an allocation.  On any failure the caller disposes of
 * the connection.
 */
static int tcp_peer_read_handshake(int sd, prte_oob_tcp_handshake_t *hs)
{
    size_t nbytes;
    int rc;

    if (!hs->sized) {
        /* the fixed part first - it says how long the nspace is */
        rc = tcp_peer_read_some(sd, (char *) &hs->hdr, PRTE_OOB_TCP_HDR_FIXED, &hs->hdr_rcvd);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
        /* then the nspace.  nslen is a single byte and PMIX_MAX_NSLEN is 255,
         * so it cannot overrun the field */
        rc = tcp_peer_read_some(sd, (char *) &hs->hdr, PRTE_OOB_TCP_HDR_LEN(&hs->hdr),
                                &hs->hdr_rcvd);
        if (PRTE_SUCCESS != rc) {
            return rc;
        }
        PRTE_OOB_TCP_HDR_END_NSPACE(&hs->hdr);
        MCA_OOB_TCP_HDR_NTOH(&hs->hdr);

        /* A handshake is the one header that carries a namespace, and this
         * is the one place it is checked - which is what lets every message
         * that follows leave it out and be reconstructed with our own (see
         * oob_tcp_hdr.h).  Only daemons of this DVM have an OOB endpoint, so
         * a peer naming any other namespace is not a peer of ours: a daemon
         * of a different DVM belonging to the same user, or a process
         * claiming to be one.  Refuse it rather than adopt it as the local
         * daemon of that rank.  An absent namespace is refused for the same
         * reason - it would fall back to ours and defeat the check. */
        if (0 == hs->hdr.nslen
            || !PMIX_CHECK_NSPACE_STRICT(hs->hdr.nspace, PRTE_PROC_MY_NAME->nspace)) {
            pmix_output(0,
                        "%s tcp_peer_recv_connect_ack: refusing a connection from "
                        "namespace \"%s\" - this daemon serves \"%s\"",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        (0 == hs->hdr.nslen) ? "(none given)" : hs->hdr.nspace,
                        PRTE_PROC_MY_NAME->nspace);
            return PRTE_ERR_CONNECTION_REFUSED;
        }

        if (MCA_OOB_TCP_PROBE == hs->hdr.type) {
            /* a probe is a header and nothing more */
            hs->sized = true;
            return PRTE_SUCCESS;
        }
        if (MCA_OOB_TCP_IDENT != hs->hdr.type) {
            pmix_output(0, "tcp_peer_recv_connect_ack: invalid header type: %d\n",
                        hs->hdr.type);
            return PRTE_ERR_COMM_FAILURE;
        }

        /* an ident's payload is the ack flag followed by a version string,
         * and everything after this reads the flag */
        nbytes = hs->hdr.nbytes;
        if (sizeof(uint16_t) > nbytes) {
            pmix_output(0, "%s tcp_peer_recv_connect_ack: a handshake of %" PRIsize_t
                        " bytes is too short to carry an acknowledgement",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), nbytes);
            return PRTE_ERR_COMM_FAILURE;
        }
        if (nbytes > (size_t) prte_oob_base.max_msg_size * 1024 * 1024) {
            pmix_proc_t sender;
            PRTE_OOB_TCP_HDR_PROC(&hs->hdr, hs->hdr.origin, &sender);
            prte_show_help(PRTE_PROC_MY_NAME->nspace, "help-oob-tcp.txt", "msg-too-big", true,
                           PRTE_NAME_PRINT(&sender), PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                           hs->hdr.nbytes, prte_oob_base.max_msg_size);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        /* one more byte than was sent, for the terminator added below */
        if (NULL == (hs->payload = (char *) malloc(nbytes + 1))) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        hs->sized = true;
    }

    if (NULL == hs->payload) {
        /* a probe */
        return PRTE_SUCCESS;
    }
    rc = tcp_peer_read_some(sd, hs->payload, hs->hdr.nbytes, &hs->payload_rcvd);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }
    hs->payload[hs->hdr.nbytes] = '\0';
    return PRTE_SUCCESS;
}

/*
 * Routine for debugging to print the connection state and socket options
 */
void prte_oob_tcp_peer_dump(prte_oob_tcp_peer_t *peer, const char *msg)
{
    char src[64];
    char dst[64];
    char buff[255];
    int sndbuf, rcvbuf, nodelay, flags;
    struct sockaddr_storage inaddr;
    prte_socklen_t addrlen = sizeof(struct sockaddr_storage);
    prte_socklen_t optlen;

    if (getsockname(peer->sd, (struct sockaddr *) &inaddr, &addrlen) < 0) {
        pmix_output(0, "tcp_peer_dump: getsockname error: %s (%d)\n",
                    strerror(prte_socket_errno), prte_socket_errno);
        snprintf(src, sizeof(src), "%s", "unknown");
    } else {
        snprintf(src, sizeof(src), "%s", pmix_net_get_hostname((struct sockaddr *) &inaddr));
    }
    if (getpeername(peer->sd, (struct sockaddr *) &inaddr, &addrlen) < 0) {
        pmix_output(0, "tcp_peer_dump: getpeername error: %s (%d)\n",
                    strerror(prte_socket_errno), prte_socket_errno);
        snprintf(dst, sizeof(dst), "%s", "unknown");
    } else {
        snprintf(dst, sizeof(dst), "%s", pmix_net_get_hostname((struct sockaddr *) &inaddr));
    }

    if ((flags = fcntl(peer->sd, F_GETFL, 0)) < 0) {
        pmix_output(0, "tcp_peer_dump: fcntl(F_GETFL) failed: %s (%d)\n",
                    strerror(prte_socket_errno), prte_socket_errno);
    }

#if defined(SO_SNDBUF)
    optlen = sizeof(sndbuf);
    if (getsockopt(peer->sd, SOL_SOCKET, SO_SNDBUF, (char *) &sndbuf, &optlen) < 0) {
        pmix_output(0, "tcp_peer_dump: SO_SNDBUF option: %s (%d)\n", strerror(prte_socket_errno),
                    prte_socket_errno);
    }
#else
    sndbuf = -1;
#endif
#if defined(SO_RCVBUF)
    optlen = sizeof(rcvbuf);
    if (getsockopt(peer->sd, SOL_SOCKET, SO_RCVBUF, (char *) &rcvbuf, &optlen) < 0) {
        pmix_output(0, "tcp_peer_dump: SO_RCVBUF option: %s (%d)\n", strerror(prte_socket_errno),
                    prte_socket_errno);
    }
#else
    rcvbuf = -1;
#endif
#if defined(TCP_NODELAY)
    optlen = sizeof(nodelay);
    if (getsockopt(peer->sd, IPPROTO_TCP, TCP_NODELAY, (char *) &nodelay, &optlen) < 0) {
        pmix_output(0, "tcp_peer_dump: TCP_NODELAY option: %s (%d)\n", strerror(prte_socket_errno),
                    prte_socket_errno);
    }
#else
    nodelay = 0;
#endif

    snprintf(buff, sizeof(buff), "%s-%s %s: %s - %s nodelay %d sndbuf %d rcvbuf %d flags %08x\n",
             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)), msg, src, dst,
             nodelay, sndbuf, rcvbuf, flags);
    pmix_output(0, "%s", buff);
}

/*
 * Accept incoming connection - if not already connected
 */

bool prte_oob_tcp_peer_accept(prte_oob_tcp_peer_t *peer)
{
    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s tcp:peer_accept called for peer %s in state %s on socket %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name),
                        prte_oob_tcp_state_print(peer->state), peer->sd);

    if (peer->state != MCA_OOB_TCP_CONNECTED) {

        tcp_peer_event_init(peer);

        if (tcp_peer_send_connect_ack(peer) != PRTE_SUCCESS) {
            pmix_output(0,
                        "%s-%s tcp_peer_accept: "
                        "tcp_peer_send_connect_ack failed\n",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)));
            peer->state = MCA_OOB_TCP_FAILED;
            prte_oob_tcp_peer_close(peer);
            return false;
        }

        tcp_peer_connected(peer);
        if (OOB_TCP_DEBUG_CONNECT
            <= pmix_output_get_verbosity(prte_oob_base.output)) {
            prte_oob_tcp_peer_dump(peer, "accepted");
        }
        return true;
    }

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s tcp:peer_accept ignored for peer %s in state %s on socket %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name),
                        prte_oob_tcp_state_print(peer->state), peer->sd);
    return false;
}
