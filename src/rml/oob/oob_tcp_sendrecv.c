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
 * Copyright (c) 2017-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Sandia National Laboratories  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * In windows, many of the socket functions return an EWOULDBLOCK
 * instead of \ things like EAGAIN, EINPROGRESS, etc. It has been
 * verified that this will \ not conflict with other error codes that
 * are returned by these functions \ under UNIX/Linux environments
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <fcntl.h>
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
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"
#include "types.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/state/state.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"

#include "src/rml/oob/oob_tcp.h"
#include "src/rml/oob/oob_tcp_common.h"
#include "src/rml/oob/oob_tcp_connection.h"
#include "src/rml/oob/oob_tcp_peer.h"

#define OOB_SEND_MAX_RETRIES 3

void prte_oob_tcp_queue_msg(int sd, short args, void *cbdata)
{
    prte_oob_tcp_send_t *snd = (prte_oob_tcp_send_t *) cbdata;
    prte_oob_tcp_peer_t *peer;
    bool connect_needed = false;
    PRTE_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(snd);
    peer = (prte_oob_tcp_peer_t *) snd->peer;

    pmix_mutex_lock(&peer->lock);
    /* if there is no message on-deck, put this one there */
    if (NULL == peer->send_msg) {
        peer->send_msg = snd;
    } else {
        /* add it to the queue */
        pmix_list_append(&peer->send_queue, &snd->super);
    }

    /* Whether or not the caller asked us to activate, a CONNECTED peer must
     * have its send event running - the last of us to observe the connection
     * is the one that has to start it.
     *
     * This is not the same test as `snd->activate`, and the difference is a
     * hang.  A message queued as *pending* (activate false) is one the send
     * path found unconnected, and the connection is being driven for it
     * already.  That used to mean "somebody else will start the send", which
     * was true while this ran on the same thread as the connection state
     * machine: tcp_peer_connected() pulled the pending message off the queue
     * and armed the event.  Now that we run on the peer's own base the two
     * interleave, and if tcp_peer_connected() gets there FIRST it finds an
     * empty queue, arms nothing, and publishes CONNECTED - after which we
     * arrive, park the message on deck, and, seeing activate false, arm
     * nothing either.  The message then sits on a connected peer with a dead
     * send event forever, which is exactly how an 8-node radix-2 launch was
     * observed to wedge with every thread idle in epoll_wait.
     */
    if (MCA_OOB_TCP_CONNECTED == peer->state) {
        if (NULL != peer->send_msg && !peer->send_ev_active) {
            peer->send_ev_active = true;
            PMIX_POST_OBJECT(peer);
            prte_event_add(&peer->send_event, 0);
        }
    } else if (snd->activate) {
        /* the peer went away between the time the send path found it
         * connected and the time we got here.  The connection state
         * machine belongs to the main progress thread, so ask for it
         * there rather than reaching into the peer's state word from
         * whichever thread is servicing this socket. */
        connect_needed = true;
    }
    pmix_mutex_unlock(&peer->lock);

    if (connect_needed) {
        PRTE_ACTIVATE_TCP_CONN_STATE(peer, prte_oob_tcp_peer_start_connect);
    }
}

static int send_msg(prte_oob_tcp_peer_t *peer, prte_oob_tcp_send_t *msg)
{
    struct iovec iov[2];
    int iov_count, retries = 0;
    ssize_t remain = msg->sdbytes, rc;

    iov[0].iov_base = msg->sdptr;
    iov[0].iov_len = msg->sdbytes;
    if (!msg->hdr_sent) {
        if (NULL != msg->data) {
            /* relay message - just send that data */
            iov[1].iov_base = msg->data;
        } else {
            /* buffer send */
            iov[1].iov_base = msg->msg->dbuf->base_ptr;
        }
        iov[1].iov_len = ntohl(msg->hdr.nbytes);
        remain += ntohl(msg->hdr.nbytes);
        iov_count = 2;
    } else {
        iov_count = 1;
    }

retry:
    rc = writev(peer->sd, iov, iov_count);
    if (PMIX_LIKELY(rc == remain)) {
        /* we successfully sent the header and the msg data if any */
        msg->hdr_sent = true;
        msg->sdbytes = 0;
        msg->sdptr = (char *) iov[iov_count - 1].iov_base + iov[iov_count - 1].iov_len;
        return PRTE_SUCCESS;
    } else if (rc < 0) {
        if (prte_socket_errno == EINTR) {
            goto retry;
        } else if (prte_socket_errno == EAGAIN) {
            /* tell the caller to keep this message on active,
             * but let the event lib cycle so other messages
             * can progress while this socket is busy
             */
            ++retries;
            if (retries < OOB_SEND_MAX_RETRIES) {
                goto retry;
            }
            return PRTE_ERR_RESOURCE_BUSY;
        } else if (prte_socket_errno == EWOULDBLOCK) {
            /* tell the caller to keep this message on active,
             * but let the event lib cycle so other messages
             * can progress while this socket is busy
             */
            ++retries;
            if (retries < OOB_SEND_MAX_RETRIES) {
                goto retry;
            }
            return PRTE_ERR_WOULD_BLOCK;
        } else {
            /* we hit an error and cannot progress this message */
            if (!prte_prteds_term_ordered && !prte_abnormal_term_ordered) {
                pmix_output(0, "oob:tcp: send_msg: write failed: %s (%d) [sd = %d]",
                            strerror(prte_socket_errno), prte_socket_errno, peer->sd);
            }
            return PRTE_ERR_UNREACH;
        }
    } else {
        /* short writev. This usually means the kernel buffer is full,
         * so there is no point for retrying at that time.
         * simply update the msg and return with PMIX_ERR_RESOURCE_BUSY */
        if ((size_t) rc < msg->sdbytes) {
            /* partial write of the header or the msg data */
            msg->sdptr = (char *) msg->sdptr + rc;
            msg->sdbytes -= rc;
        } else {
            /* header was fully written, but only a part of the msg data was written */
            msg->hdr_sent = true;
            rc -= msg->sdbytes;
            assert(2 == iov_count);
            msg->sdptr = (char *) iov[1].iov_base + rc;
            msg->sdbytes = ntohl(msg->hdr.nbytes) - rc;
        }
        return PRTE_ERR_RESOURCE_BUSY;
    }
}

/*
 * A file descriptor is available/ready for send. Check the state
 * of the socket and take the appropriate action.
 */
void prte_oob_tcp_send_handler(int sd, short flags, void *cbdata)
{
    prte_oob_tcp_peer_t *peer = (prte_oob_tcp_peer_t *) cbdata;
    prte_oob_tcp_send_t *msg;
    int rc;
    PRTE_HIDE_UNUSED_PARAMS(sd, flags);

    PMIX_ACQUIRE_OBJECT(peer);
    /* the on-deck message can be swapped out from under us by a close running
     * on the main progress thread, so take a guarded look at it */
    pmix_mutex_lock(&peer->lock);
    msg = peer->send_msg;
    pmix_mutex_unlock(&peer->lock);

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s tcp:send_handler called to send to peer %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name));

    switch (peer->state) {
    case MCA_OOB_TCP_CONNECTING:
    case MCA_OOB_TCP_CLOSED:
        /* the socket is still being connected, so its events are still bound
         * to prte_event_base - the connection state machine never runs on a
         * worker base */
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s tcp:send_handler %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            prte_oob_tcp_state_print(peer->state));
        prte_oob_tcp_peer_complete_connect(peer);
        /* de-activate the send event until the connection
         * handshake completes
         */
        if (peer->send_ev_active) {
            prte_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }
        break;
    case MCA_OOB_TCP_CONNECTED:
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s tcp:send_handler SENDING TO %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                            (NULL == peer->send_msg) ? "NULL" : PRTE_NAME_PRINT(&peer->name));
        if (NULL != msg) {
            pmix_output_verbose(2, prte_oob_base.output,
                                "oob:tcp:send_handler SENDING MSG");
            if (PRTE_SUCCESS == (rc = send_msg(peer, msg))) {
                /* this msg is complete */
                if (NULL != msg->data || NULL == msg->msg) {
                    /* the relay is complete - release the data */
                    pmix_output_verbose(2, prte_oob_base.output,
                                        "%s MESSAGE RELAY COMPLETE TO %s OF %d BYTES ON SOCKET %d",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                        PRTE_NAME_PRINT(&(peer->name)),
                                        (int) ntohl(msg->hdr.nbytes), peer->sd);
                    pmix_mutex_lock(&peer->lock);
                    peer->send_msg = NULL;
                    pmix_mutex_unlock(&peer->lock);
                    PMIX_RELEASE(msg);
                } else {
                    /* we are done - notify the RML */
                    prte_rml_send_t *snd = msg->msg;
                    pmix_output_verbose(2, prte_oob_base.output,
                                        "%s MESSAGE SEND COMPLETE TO %s OF %d BYTES ON SOCKET %d",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                        PRTE_NAME_PRINT(&(peer->name)),
                                        (int) ntohl(msg->hdr.nbytes), peer->sd);
                    snd->status = PRTE_SUCCESS;
                    msg->msg = NULL; // the completion owns it now
                    pmix_mutex_lock(&peer->lock);
                    peer->send_msg = NULL;
                    pmix_mutex_unlock(&peer->lock);
                    PMIX_RELEASE(msg);
                    /* the callback runs on the main progress thread - never
                     * under the peer lock, and never on a worker base */
                    PRTE_OOB_COMPLETE_SEND(peer, snd);
                }
                /* fall thru to queue the next message */
            } else if (PRTE_ERR_RESOURCE_BUSY == rc || PRTE_ERR_WOULD_BLOCK == rc) {
                /* exit this event and let the event lib progress */
                return;
            } else {
                // report the error
                if (!prte_prteds_term_ordered && !prte_abnormal_term_ordered) {
                    pmix_output(
                        0, "%s-%s prte_oob_tcp_peer_send_handler: "
                        "unable to send message ON SOCKET %d",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                        PRTE_NAME_PRINT(&(peer->name)), peer->sd);
                }
                prte_oob_tcp_peer_close(peer);
                return;
            }

            /* if current message completed - progress any pending sends by
             * moving the next in the queue into the "on-deck" position. Note
             * that this doesn't mean we send the message right now - we will
             * wait for another send_event to fire before doing so. This gives
             * us a chance to service any pending recvs.
             */
            pmix_mutex_lock(&peer->lock);
            if (NULL == peer->send_msg) {
                peer->send_msg = (prte_oob_tcp_send_t *) pmix_list_remove_first(&peer->send_queue);
            }
            pmix_mutex_unlock(&peer->lock);
        }

        /* if nothing else to do unregister for send event notifications */
        pmix_mutex_lock(&peer->lock);
        if (NULL == peer->send_msg && peer->send_ev_active) {
            prte_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }
        pmix_mutex_unlock(&peer->lock);
        break;
    default:
        pmix_output(
            0, "%s-%s prte_oob_tcp_peer_send_handler: invalid connection state (%d) on socket %d",
            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)), peer->state,
            peer->sd);
        pmix_mutex_lock(&peer->lock);
        if (peer->send_ev_active) {
            prte_event_del(&peer->send_event);
            peer->send_ev_active = false;
        }
        pmix_mutex_unlock(&peer->lock);
        break;
    }
}

static int read_bytes(prte_oob_tcp_peer_t *peer)
{
    int rc;

    /* read until all bytes recvd or error */
    while (0 < peer->recv_msg->rdbytes) {
        rc = read(peer->sd, peer->recv_msg->rdptr, peer->recv_msg->rdbytes);
        if (rc < 0) {
            if (prte_socket_errno == EINTR) {
                continue;
            } else if (prte_socket_errno == EAGAIN) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return PRTE_ERR_RESOURCE_BUSY;
            } else if (prte_socket_errno == EWOULDBLOCK) {
                /* tell the caller to keep this message on active,
                 * but let the event lib cycle so other messages
                 * can progress while this socket is busy
                 */
                return PRTE_ERR_WOULD_BLOCK;
            }
            /* we hit an error and cannot progress this message - report
             * the error back to the RML and let the caller know
             * to abort this message
             */
            pmix_output_verbose(OOB_TCP_DEBUG_FAIL, prte_oob_base.output,
                                "%s-%s prte_oob_tcp_msg_recv: readv failed: %s (%d)",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                                strerror(prte_socket_errno), prte_socket_errno);
            // if (NULL != mca_oob_tcp.oob_exception_callback) {
            // mca_oob_tcp.oob_exception_callback(&peer->name, PRTE_RML_PEER_DISCONNECTED);
            //}
            return PRTE_ERR_COMM_FAILURE;
        } else if (rc == 0) {
            /* the remote peer closed the connection - report that condition
             * and let the caller know
             */
            pmix_output_verbose(OOB_TCP_DEBUG_FAIL, prte_oob_base.output,
                                "%s-%s prte_oob_tcp_msg_recv: peer closed connection",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)));
            // if (NULL != mca_oob_tcp.oob_exception_callback) {
            //   mca_oob_tcp.oob_exception_callback(&peer->peer_name, PRTE_RML_PEER_DISCONNECTED);
            //}
            return PRTE_ERR_COMM_FAILURE;
        }
        /* we were able to read something, so adjust counters and location */
        peer->recv_msg->rdbytes -= rc;
        peer->recv_msg->rdptr += rc;
    }

    /* we read the full data block */
    return PRTE_SUCCESS;
}

/*
 * Dispatch to the appropriate action routine based on the state
 * of the connection with the peer.
 */

void prte_oob_tcp_recv_handler(int sd, short flags, void *cbdata)
{
    prte_oob_tcp_peer_t *peer = (prte_oob_tcp_peer_t *) cbdata;
    int rc;
    prte_rml_send_t *snd;
    pmix_byte_object_t bo;
    PRTE_HIDE_UNUSED_PARAMS(sd, flags);

    PMIX_ACQUIRE_OBJECT(peer);

    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                        "%s:tcp:recv:handler called for peer %s",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name));

    switch (peer->state) {
    case MCA_OOB_TCP_CONNECT_ACK:
        if (PRTE_SUCCESS == (rc = prte_oob_tcp_peer_recv_connect_ack(peer, peer->sd, NULL))) {
            pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                "%s:tcp:recv:handler starting send/recv events",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
            /* we connected! Start the send/recv events.
             *
             * tcp_peer_connected() has already run beneath the ack: the peer
             * is CONNECTED, its socket events now live on peer->evbase, and a
             * worker may already be queueing onto it.  So everything below
             * that touches the send queue or the send event has to be guarded
             * - it is no longer this thread's alone. */
            if (!peer->recv_ev_active) {
                peer->recv_ev_active = true;
                PMIX_POST_OBJECT(peer);
                prte_event_add(&peer->recv_event, 0);
            }
            if (peer->timer_ev_active) {
                prte_event_del(&peer->timer_event);
                peer->timer_ev_active = false;
            }
            /* if there is a message waiting to be sent, queue it */
            pmix_mutex_lock(&peer->lock);
            if (NULL == peer->send_msg) {
                peer->send_msg = (prte_oob_tcp_send_t *) pmix_list_remove_first(&peer->send_queue);
            }
            if (NULL != peer->send_msg && !peer->send_ev_active) {
                peer->send_ev_active = true;
                PMIX_POST_OBJECT(peer);
                prte_event_add(&peer->send_event, 0);
            }
            /* update our state */
            peer->state = MCA_OOB_TCP_CONNECTED;
            pmix_mutex_unlock(&peer->lock);
        } else if (PRTE_ERR_UNREACH != rc) {
            /* we get an unreachable error returned if a connection
             * completes but is rejected - otherwise, we don't want
             * to terminate as we might be retrying the connection */
            pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                "%s UNABLE TO COMPLETE CONNECT ACK WITH %s",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name));
            prte_oob_tcp_peer_close(peer);
            return;
        }
        break;
    case MCA_OOB_TCP_CONNECTED:
        pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                            "%s:tcp:recv:handler CONNECTED", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        /* allocate a new message and setup for recv */
        if (NULL == peer->recv_msg) {
            pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                "%s:tcp:recv:handler allocate new recv msg",
                                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
            peer->recv_msg = PMIX_NEW(prte_oob_tcp_recv_t);
            if (NULL == peer->recv_msg) {
                pmix_output(
                    0, "%s-%s prte_oob_tcp_peer_recv_handler: unable to allocate recv message\n",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)));
                return;
            }
            /* start by reading the fixed part of the header - the nspace
             * that trails it is only as long as the header says it is, and
             * the header has to be in hand to know that */
            peer->recv_msg->rdptr = (char *) &peer->recv_msg->hdr;
            peer->recv_msg->rdbytes = PRTE_OOB_TCP_HDR_FIXED;
        }
        /* if the header hasn't been completely read, read it */
        if (!peer->recv_msg->hdr_recvd) {
            pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                "%s:tcp:recv:handler read hdr", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
            if (PRTE_SUCCESS == (rc = read_bytes(peer))) {
                /* completed reading the fixed part of the header */
                peer->recv_msg->hdr_recvd = true;
                /* convert the header */
                MCA_OOB_TCP_HDR_NTOH(&peer->recv_msg->hdr);
                /* set up to read the nspace that trails it.  nslen is a
                 * single byte and PMIX_MAX_NSLEN is 255, so it cannot name
                 * more characters than the field can hold */
                peer->recv_msg->rdptr = peer->recv_msg->hdr.nspace;
                peer->recv_msg->rdbytes = peer->recv_msg->hdr.nslen;
                /* fall thru and attempt to read it */
            } else if (PRTE_ERR_RESOURCE_BUSY == rc || PRTE_ERR_WOULD_BLOCK == rc) {
                /* exit this event and let the event lib progress */
                return;
            } else {
                /* close the connection */
                pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                    "%s:tcp:recv:handler error reading bytes - closing connection",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                prte_oob_tcp_peer_close(peer);
                return;
            }
        }

        /* the names in this header are a rank plus the nspace that follows the
         * fixed part, so nothing may read one until those characters are in */
        if (peer->recv_msg->hdr_recvd && !peer->recv_msg->nspace_recvd) {
            if (0 < peer->recv_msg->hdr.nslen) {
                rc = read_bytes(peer);
                if (PRTE_ERR_RESOURCE_BUSY == rc || PRTE_ERR_WOULD_BLOCK == rc) {
                    /* exit this event and let the event lib progress */
                    return;
                }
                if (PRTE_SUCCESS != rc) {
                    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                        "%s:tcp:recv:handler error reading nspace - closing connection",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
                    prte_oob_tcp_peer_close(peer);
                    return;
                }
            }
            /* the terminator is not sent - we supply it */
            PRTE_OOB_TCP_HDR_END_NSPACE(&peer->recv_msg->hdr);
            peer->recv_msg->nspace_recvd = true;

            /* if this is a zero-byte message, then we are done */
            if (0 == peer->recv_msg->hdr.nbytes) {
                pmix_output_verbose(OOB_TCP_DEBUG_CONNECT,
                                    prte_oob_base.output,
                                    "%s RECVD ZERO-BYTE MESSAGE FROM %s for tag %d",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    PRTE_NAME_PRINT(&peer->name), peer->recv_msg->hdr.tag);
                peer->recv_msg->data = NULL; // make sure
            } else {
                pmix_output_verbose(OOB_TCP_DEBUG_CONNECT,
                                    prte_oob_base.output,
                                    "%s:tcp:recv:handler allocate data region of size %lu",
                                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    (unsigned long) peer->recv_msg->hdr.nbytes);
                if (peer->recv_msg->hdr.nbytes > (uint32_t)(prte_oob_base.max_msg_size * 1024 * 1024)) {
                    prte_show_help("help-oob-tcp.txt", "msg-too-big", true,
                                    PRTE_NAME_PRINT(&peer->name), PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                    peer->recv_msg->hdr.nbytes, prte_oob_base.max_msg_size);
                    peer->state = MCA_OOB_TCP_FAILED;
                    prte_oob_tcp_peer_close(peer);
                    return;
                }
                /* allocate the data region */
                peer->recv_msg->data = (char *) malloc(peer->recv_msg->hdr.nbytes);
                /* point to it */
                peer->recv_msg->rdptr = peer->recv_msg->data;
                peer->recv_msg->rdbytes = peer->recv_msg->hdr.nbytes;
            }
            /* fall thru and attempt to read the data */
        }

        if (peer->recv_msg->nspace_recvd) {
            pmix_proc_t origin, dest;

            /* the header carries the two ranks and the nspace they share */
            PRTE_OOB_TCP_HDR_PROC(&peer->recv_msg->hdr, peer->recv_msg->hdr.origin, &origin);
            PRTE_OOB_TCP_HDR_PROC(&peer->recv_msg->hdr, peer->recv_msg->hdr.dst, &dest);

            /* continue to read the data block - we start from
             * wherever we left off, which could be at the
             * beginning or somewhere in the message
             */
            if (PRTE_SUCCESS == (rc = read_bytes(peer))) {
                /* we recvd all of the message */
                pmix_output_verbose(
                    OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                    "%s RECVD COMPLETE MESSAGE FROM %s (ORIGIN %s) OF %d BYTES FOR DEST %s TAG %d",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&peer->name),
                    PRTE_NAME_PRINT(&origin), (int) peer->recv_msg->hdr.nbytes,
                    PRTE_NAME_PRINT(&dest), peer->recv_msg->hdr.tag);

                /* Incarnation guard (bootstrap only): a departed daemon can
                 * reboot into the same rank and return with a strictly-greater
                 * boot epoch. Drop traffic stamped with a stale (older) epoch
                 * for a daemon rank so the old incarnation's late messages are
                 * not confused with the new one. Only daemon-namespace traffic
                 * is checked -- tool namespaces reuse rank numbers. The full
                 * message has already been read off the socket, so dropping
                 * here leaves the byte stream correctly framed. */
                if (prte_bootstrap_setup &&
                    PMIX_CHECK_NSPACE(origin.nspace, PRTE_PROC_MY_NAME->nspace) &&
                    !prte_rml_epoch_ok(origin.rank, peer->recv_msg->hdr.epoch)) {
                    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT, prte_oob_base.output,
                                        "%s DROPPING STALE-INCARNATION MSG FROM %s epoch %lu",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                        PRTE_NAME_PRINT(&origin),
                                        (unsigned long) peer->recv_msg->hdr.epoch);
                    PMIX_RELEASE(peer->recv_msg);
                    peer->recv_msg = NULL;
                    return;
                }

                /* am I the intended recipient (header was already converted back to host order)? */
                if (PMIX_CHECK_PROCID(&dest, PRTE_PROC_MY_NAME)) {
                    /* yes - post it to the RML for delivery */
                    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT,
                                        prte_oob_base.output,
                                        "%s DELIVERING TO RML tag = %d seq_num = %d",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), peer->recv_msg->hdr.tag,
                                        peer->recv_msg->hdr.seq_num);
                    PRTE_RML_POST_MESSAGE(&origin, peer->recv_msg->hdr.tag,
                                          peer->recv_msg->hdr.seq_num, peer->recv_msg->data,
                                          peer->recv_msg->hdr.nbytes);
                    /* PMIx_Data_load took ownership of the payload */
                    peer->recv_msg->data = NULL;
                    PMIX_RELEASE(peer->recv_msg);
                } else {
                    /* not for us - we are an intermediate hop. Re-enter the
                     * OOB send path, which will route the message on toward
                     * its final destination via the next hop in the tree.
                     */
                    pmix_output_verbose(OOB_TCP_DEBUG_CONNECT,
                                        prte_oob_base.output,
                                        "%s TCP RELAYING ROUTED MESSAGE FOR %s",
                                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                        PRTE_NAME_PRINT(&dest));
                    snd = PMIX_NEW(prte_rml_send_t);
                    PMIX_XFER_PROCID(&snd->dst, &dest);
                    PMIX_XFER_PROCID(&snd->origin, &origin);
                    /* preserve the origin's epoch across the relay (the
                     * constructor defaulted it to our own epoch) */
                    snd->epoch = peer->recv_msg->hdr.epoch;
                    snd->tag = peer->recv_msg->hdr.tag;
                    bo.bytes = peer->recv_msg->data;
                    bo.size = peer->recv_msg->hdr.nbytes;
                    PMIX_DATA_BUFFER_CREATE(snd->dbuf);
                    rc = PMIx_Data_load(snd->dbuf, &bo);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                    }
                    snd->seq_num = peer->recv_msg->hdr.seq_num;
                    /* activate the OOB send state */
                    PRTE_OOB_SEND(snd);
                    /* cleanup */
                    peer->recv_msg->data = NULL;
                    PMIX_RELEASE(peer->recv_msg);
                }
                peer->recv_msg = NULL;
                return;
            } else if (PRTE_ERR_RESOURCE_BUSY == rc || PRTE_ERR_WOULD_BLOCK == rc) {
                /* exit this event and let the event lib progress */
                return;
            } else {
                /* report the error and close the connection */
                pmix_output(0, "%s-%s prte_oob_tcp_peer_recv_handler: unable to recv message",
                            PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)));
                prte_oob_tcp_peer_close(peer);
                return;
            }
        }
        break;
    default:
        pmix_output(0, "%s-%s prte_oob_tcp_peer_recv_handler: invalid socket state(%d)",
                    PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), PRTE_NAME_PRINT(&(peer->name)),
                    peer->state);
        prte_oob_tcp_peer_close(peer);
        break;
    }
}

static void snd_cons(prte_oob_tcp_send_t *ptr)
{
    memset(&ptr->hdr, 0, sizeof(prte_oob_tcp_hdr_t));
    ptr->msg = NULL;
    ptr->data = NULL;
    ptr->hdr_sent = false;
    ptr->iovnum = 0;
    ptr->sdptr = NULL;
    ptr->sdbytes = 0;
}
/* an OOB send owns the RML message attached to it until that message is
 * completed - completion clears the pointer, so anything still attached
 * here (a send abandoned when its peer was torn down) is ours to release.
 * If we relay a msg, the data in the relay belongs to us as well and must
 * be free'd
 */
static void snd_des(prte_oob_tcp_send_t *ptr)
{
    if (NULL != ptr->msg) {
        PMIX_RELEASE(ptr->msg);
    }
    if (NULL != ptr->data) {
        free(ptr->data);
    }
}
PMIX_CLASS_INSTANCE(prte_oob_tcp_send_t, pmix_list_item_t, snd_cons, snd_des);

static void rcv_cons(prte_oob_tcp_recv_t *ptr)
{
    memset(&ptr->hdr, 0, sizeof(prte_oob_tcp_hdr_t));
    ptr->hdr_recvd = false;
    ptr->nspace_recvd = false;
    ptr->data = NULL;
    ptr->rdptr = NULL;
    ptr->rdbytes = 0;
}
/* the payload buffer belongs to the recv object until somebody takes it: the
 * two paths that hand it on (deliver locally, relay onward) clear the pointer
 * first, so anything still here - a message dropped as a stale incarnation, or
 * a partial recv abandoned when the peer was closed - is ours to free
 */
static void rcv_des(prte_oob_tcp_recv_t *ptr)
{
    if (NULL != ptr->data) {
        free(ptr->data);
    }
}
PMIX_CLASS_INSTANCE(prte_oob_tcp_recv_t, pmix_list_item_t, rcv_cons, rcv_des);
