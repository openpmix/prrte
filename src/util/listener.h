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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_LISTENER_H
#define PRRTE_LISTENER_H

#include "prrte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include "src/class/prrte_list.h"
#include "src/event/event-internal.h"
#include "src/include/types.h"

/* callback prototype */
typedef void (*prrte_listener_callback_fn_t)(int sd, short args, void *cbdata);

/*
 * Data structure for accepting connections.
 */
typedef struct prrte_listener_t {
    prrte_list_item_t item;
    int sd;
    prrte_event_base_t *evbase;
    prrte_listener_callback_fn_t handler;
} prrte_listener_t;
PRRTE_CLASS_DECLARATION(prrte_listener_t);

typedef struct {
    prrte_object_t super;
    prrte_event_t ev;
    int fd;
    struct sockaddr_storage addr;
} prrte_pending_connection_t;
PRRTE_CLASS_DECLARATION(prrte_pending_connection_t);

PRRTE_EXPORT int prrte_start_listening(void);
PRRTE_EXPORT void prrte_stop_listening(void);
PRRTE_EXPORT int prrte_register_listener(struct sockaddr* address, prrte_socklen_t addrlen,
                                         prrte_event_base_t *evbase,
                                         prrte_listener_callback_fn_t handler);

#endif /* PRRTE_LISTENER_H */
