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
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _MCA_OOB_TCP_PEER_H_
#define _MCA_OOB_TCP_PEER_H_

#include "prrte_config.h"

#include "src/event/event-internal.h"

#include "src/threads/threads.h"
#include "oob_tcp.h"
#include "oob_tcp_sendrecv.h"

typedef struct {
    prrte_list_item_t super;
    struct sockaddr_storage addr; // an address where a peer can be found
    int retries;                  // number of times we have tried to connect to this address
    prrte_oob_tcp_state_t state;    // state of this address
} prrte_oob_tcp_addr_t;
PRRTE_CLASS_DECLARATION(prrte_oob_tcp_addr_t);

/* object for tracking peers in the module */
typedef struct {
    prrte_list_item_t super;
    /* although not required, there is enough debug
     * value that retaining the name makes sense
     */
    prrte_process_name_t name;
    char *auth_method;  // method they used to authenticate
    int sd;
    prrte_list_t addrs;
    prrte_oob_tcp_addr_t *active_addr;
    prrte_oob_tcp_state_t state;
    int num_retries;
    prrte_event_t send_event;    /**< registration with event thread for send events */
    bool send_ev_active;
    prrte_event_t recv_event;    /**< registration with event thread for recv events */
    bool recv_ev_active;
    prrte_event_t timer_event;   /**< timer for retrying connection failures */
    bool timer_ev_active;
    prrte_list_t send_queue;      /**< list of messages to send */
    prrte_oob_tcp_send_t *send_msg; /**< current send in progress */
    prrte_oob_tcp_recv_t *recv_msg; /**< current recv in progress */
} prrte_oob_tcp_peer_t;
PRRTE_CLASS_DECLARATION(prrte_oob_tcp_peer_t);

/* state machine for processing peer data */
typedef struct {
    prrte_object_t super;
    prrte_event_t ev;
    prrte_process_name_t peer;
    uint16_t af_family;
    char *net;
    char *port;
} prrte_oob_tcp_peer_op_t;
PRRTE_CLASS_DECLARATION(prrte_oob_tcp_peer_op_t);

#define PRRTE_ACTIVATE_TCP_CMP_OP(p, cbfunc)                          \
    do {                                                                \
        prrte_oob_tcp_peer_op_t *pop;                                     \
        pop = PRRTE_NEW(prrte_oob_tcp_peer_op_t);                           \
        pop->peer.jobid = (p)->name.jobid;                              \
        pop->peer.vpid = (p)->name.vpid;                                \
        PRRTE_THREADSHIFT(pop, prrte_event_base,                    \
                         (cbfunc), PRRTE_MSG_PRI);                       \
    } while(0);

#endif /* _MCA_OOB_TCP_PEER_H_ */
