/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _MCA_OOB_TCP_SENDRECV_H_
#define _MCA_OOB_TCP_SENDRECV_H_

#include "prte_config.h"

#include "src/class/pmix_list.h"
#include "src/util/pmix_string_copy.h"

#include "src/rml/oob/oob_tcp.h"
#include "src/rml/oob/oob_tcp_hdr.h"
#include "src/rml/rml.h"
#include "src/threads/pmix_threads.h"

/* forward declare */
struct prte_oob_tcp_peer_t;

/* tcp structure for sending a message */
typedef struct {
    pmix_list_item_t super;
    prte_event_t ev;
    struct prte_oob_tcp_peer_t *peer;
    bool activate;
    prte_oob_tcp_hdr_t hdr;
    prte_rml_send_t *msg;
    char *data;
    bool hdr_sent;
    int iovnum;
    char *sdptr;
    size_t sdbytes;
} prte_oob_tcp_send_t;
PMIX_CLASS_DECLARATION(prte_oob_tcp_send_t);

/* tcp structure for recving a message */
typedef struct {
    pmix_list_item_t super;
    prte_oob_tcp_hdr_t hdr;
    bool hdr_recvd;
    char *data;
    char *rdptr;
    size_t rdbytes;
} prte_oob_tcp_recv_t;
PMIX_CLASS_DECLARATION(prte_oob_tcp_recv_t);

/* Queue a message to be sent to a specified peer. The macro
 * checks to see if a message is already in position to be
 * sent - if it is, then the message provided is simply added
 * to the peer's message queue. If not, then the provided message
 * is placed in the "ready" position
 *
 * If the provided boolean is true, then the send event for the
 * peer is checked and activated if not already active. This allows
 * the macro to either immediately send the message, or to queue
 * it as "pending" for later transmission - e.g., after the
 * connection procedure is completed
 *
 * p => pointer to prte_oob_tcp_peer_t
 * s => pointer to prte_oob_tcp_send_t
 * f => true if send event is to be activated
 */
#define MCA_OOB_TCP_QUEUE_MSG(p, s, f)                                          \
    do {                                                                        \
        (s)->peer = (struct prte_oob_tcp_peer_t *) (p);                         \
        (s)->activate = (f);                                                    \
        PRTE_PMIX_THREADSHIFT((s), prte_event_base, prte_oob_tcp_queue_msg);    \
    } while (0)

/* queue a message for transmission to a connected peer - must
 * provide the following params:
 *
 * m - the RML message to be sent
 * p - the peer (next hop) to send it to
 */
#define MCA_OOB_TCP_QUEUE_SEND(m, p)                                                           \
    do {                                                                                       \
        prte_oob_tcp_send_t *_s;                                                               \
        pmix_output_verbose(5, prte_oob_base.output,                       \
                            "%s:[%s:%d] queue send to %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), \
                            __FILE__, __LINE__, PRTE_NAME_PRINT(&((m)->dst)));                 \
        _s = PMIX_NEW(prte_oob_tcp_send_t);                                                    \
        /* setup the header */                                                                 \
        PMIX_XFER_PROCID(&_s->hdr.origin, &(m)->origin);                                       \
        PMIX_XFER_PROCID(&_s->hdr.dst, &(m)->dst);                                             \
        _s->hdr.type = MCA_OOB_TCP_USER;                                                       \
        _s->hdr.tag = (m)->tag;                                                                \
        _s->hdr.seq_num = (m)->seq_num;                                                        \
        _s->hdr.epoch = (m)->epoch;                                                            \
        /* point to the actual message */                                                      \
        _s->msg = (m);                                                                         \
        /* set the total number of bytes to be sent */                                         \
        _s->hdr.nbytes = (m)->dbuf->bytes_used;                                                 \
        /* prep header for xmission */                                                         \
        MCA_OOB_TCP_HDR_HTON(&_s->hdr);                                                        \
        /* start the send with the header */                                                   \
        _s->sdptr = (char *) &_s->hdr;                                                         \
        _s->sdbytes = sizeof(prte_oob_tcp_hdr_t);                                              \
        /* add to the msg queue for this peer */                                               \
        MCA_OOB_TCP_QUEUE_MSG((p), _s, true);                                                  \
    } while (0)

/* queue a message to be sent to a peer once its connection has finished
 * being established - must provide the following params:
 *
 * m - the RML message to be sent
 * p - the peer (next hop) to send it to
 */
#define MCA_OOB_TCP_QUEUE_PENDING(m, p)                                                           \
    do {                                                                                          \
        prte_oob_tcp_send_t *_s;                                                                  \
        pmix_output_verbose(5, prte_oob_base.output,                          \
                            "%s:[%s:%d] queue pending to %s", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), \
                            __FILE__, __LINE__, PRTE_NAME_PRINT(&((m)->dst)));                    \
        _s = PMIX_NEW(prte_oob_tcp_send_t);                                                       \
        /* setup the header */                                                                    \
        PMIX_XFER_PROCID(&_s->hdr.origin, &(m)->origin);                                          \
        PMIX_XFER_PROCID(&_s->hdr.dst, &(m)->dst);                                                \
        _s->hdr.type = MCA_OOB_TCP_USER;                                                          \
        _s->hdr.tag = (m)->tag;                                                                   \
        _s->hdr.seq_num = (m)->seq_num;                                                           \
        _s->hdr.epoch = (m)->epoch;                                                               \
        /* point to the actual message */                                                         \
        _s->msg = (m);                                                                            \
        /* set the total number of bytes to be sent */                                            \
        _s->hdr.nbytes = (m)->dbuf->bytes_used;                                                    \
        /* prep header for xmission */                                                            \
        MCA_OOB_TCP_HDR_HTON(&_s->hdr);                                                           \
        /* start the send with the header */                                                      \
        _s->sdptr = (char *) &_s->hdr;                                                            \
        _s->sdbytes = sizeof(prte_oob_tcp_hdr_t);                                                 \
        /* add to the msg queue for this peer */                                                  \
        MCA_OOB_TCP_QUEUE_MSG((p), _s, false);                                                    \
    } while (0)

#endif /* _MCA_OOB_TCP_SENDRECV_H_ */
