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
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRTE_LISTENER_H
#define PRTE_LISTENER_H

#include "prte_config.h"

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif

#include "src/class/prte_list.h"
#include "src/event/event-internal.h"
#include "src/include/types.h"

/* callback prototype */
typedef void (*prte_listener_callback_fn_t)(int sd, short args, void *cbdata);

/*
 * Data structure for accepting connections.
 */
typedef struct prte_listener_t {
    prte_list_item_t item;
    int sd;
    prte_event_base_t *evbase;
    prte_listener_callback_fn_t handler;
} prte_listener_t;
PRTE_CLASS_DECLARATION(prte_listener_t);

typedef struct {
    prte_object_t super;
    prte_event_t ev;
    int fd;
    struct sockaddr_storage addr;
} prte_pending_connection_t;
PRTE_CLASS_DECLARATION(prte_pending_connection_t);

PRTE_EXPORT int prte_start_listening(void);
PRTE_EXPORT void prte_stop_listening(void);
PRTE_EXPORT int prte_register_listener(struct sockaddr *address, prte_socklen_t addrlen,
                                       prte_event_base_t *evbase,
                                       prte_listener_callback_fn_t handler);

#endif /* PRTE_LISTENER_H */
