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
 * Copyright (c) 2010-2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _MCA_OOB_TCP_CONNECTION_H_
#define _MCA_OOB_TCP_CONNECTION_H_

#include "prrte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include "src/threads/threads.h"
#include "oob_tcp.h"
#include "oob_tcp_peer.h"

/* State machine for connection operations */
typedef struct {
    prrte_object_t super;
    prrte_oob_tcp_peer_t *peer;
    prrte_event_t ev;
} prrte_oob_tcp_conn_op_t;
PRRTE_CLASS_DECLARATION(prrte_oob_tcp_conn_op_t);

#define CLOSE_THE_SOCKET(socket)    \
    do {                            \
        shutdown(socket, 2);        \
        close(socket);              \
    } while(0)

#define PRRTE_ACTIVATE_TCP_CONN_STATE(p, cbfunc)                         \
    do {                                                                \
        prrte_oob_tcp_conn_op_t *cop;                                     \
        prrte_output_verbose(5, prrte_oob_base_framework.framework_output, \
                            "%s:[%s:%d] connect to %s",                 \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),         \
                            __FILE__, __LINE__,                         \
                            PRRTE_NAME_PRINT((&(p)->name)));             \
        cop = PRRTE_NEW(prrte_oob_tcp_conn_op_t);                           \
        cop->peer = (p);                                                \
        PRRTE_THREADSHIFT(cop, prrte_event_base, (cbfunc), PRRTE_MSG_PRI);    \
    } while(0);

#define PRRTE_ACTIVATE_TCP_ACCEPT_STATE(s, a, cbfunc)            \
    do {                                                        \
        prrte_oob_tcp_conn_op_t *cop;                             \
        cop = PRRTE_NEW(prrte_oob_tcp_conn_op_t);                   \
        prrte_event_set(prrte_event_base, &cop->ev, s,      \
                       PRRTE_EV_READ, (cbfunc), cop);            \
        prrte_event_set_priority(&cop->ev, PRRTE_MSG_PRI);        \
        PRRTE_POST_OBJECT(cop);                                  \
        prrte_event_add(&cop->ev, 0);                            \
    } while(0);

#define PRRTE_RETRY_TCP_CONN_STATE(p, cbfunc, tv)                        \
    do {                                                                \
        prrte_oob_tcp_conn_op_t *cop;                                     \
        prrte_output_verbose(5, prrte_oob_base_framework.framework_output, \
                            "%s:[%s:%d] retry connect to %s",           \
                            PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),         \
                            __FILE__, __LINE__,                         \
                            PRRTE_NAME_PRINT((&(p)->name)));             \
        cop = PRRTE_NEW(prrte_oob_tcp_conn_op_t);                           \
        cop->peer = (p);                                                \
        prrte_event_evtimer_set(prrte_event_base,                            \
                               &cop->ev,                                \
                               (cbfunc), cop);                          \
        PRRTE_POST_OBJECT(cop);                                          \
        prrte_event_evtimer_add(&cop->ev, (tv));                         \
    } while(0);

PRRTE_MODULE_EXPORT void prrte_oob_tcp_peer_try_connect(int fd, short args, void *cbdata);
PRRTE_MODULE_EXPORT void prrte_oob_tcp_peer_dump(prrte_oob_tcp_peer_t* peer, const char* msg);
PRRTE_MODULE_EXPORT bool prrte_oob_tcp_peer_accept(prrte_oob_tcp_peer_t* peer);
PRRTE_MODULE_EXPORT void prrte_oob_tcp_peer_complete_connect(prrte_oob_tcp_peer_t* peer);
PRRTE_MODULE_EXPORT int prrte_oob_tcp_peer_recv_connect_ack(prrte_oob_tcp_peer_t* peer,
                                                           int sd, prrte_oob_tcp_hdr_t *dhdr);
PRRTE_MODULE_EXPORT void prrte_oob_tcp_peer_close(prrte_oob_tcp_peer_t *peer);

#endif /* _MCA_OOB_TCP_CONNECTION_H_ */
