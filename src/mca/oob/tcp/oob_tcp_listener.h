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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef _MCA_OOB_TCP_LISTENER_H_
#define _MCA_OOB_TCP_LISTENER_H_

#include "prrte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include "src/class/prrte_list.h"
#include "src/event/event-internal.h"

/*
 * Data structure for accepting connections.
 */
struct prrte_oob_tcp_listener_t {
    prrte_list_item_t item;
    bool ev_active;
    prrte_event_t event;
    bool tcp6;
    int sd;
    uint16_t port;
};
typedef struct prrte_oob_tcp_listener_t prrte_oob_tcp_listener_t;
PRRTE_CLASS_DECLARATION(prrte_oob_tcp_listener_t);

typedef struct {
    prrte_object_t super;
    prrte_event_t ev;
    int fd;
    struct sockaddr_storage addr;
} prrte_oob_tcp_pending_connection_t;
PRRTE_CLASS_DECLARATION(prrte_oob_tcp_pending_connection_t);

PRRTE_MODULE_EXPORT int prrte_oob_tcp_start_listening(void);

#endif /* _MCA_OOB_TCP_LISTENER_H_ */
