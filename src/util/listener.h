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
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_LISTENER_H
#define PMIX_LISTENER_H

#include "prte_config.h"

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif

#include "src/class/pmix_list.h"
#include "src/event/event-internal.h"
#include "src/include/types.h"

/* callback prototype */
typedef void (*pmix_listener_callback_fn_t)(int sd, short args, void *cbdata);

/*
 * Data structure for accepting connections.
 */
typedef struct pmix_listener_t {
    pmix_list_item_t item;
    int sd;
    prte_event_base_t *evbase;
    pmix_listener_callback_fn_t handler;
} pmix_listener_t;
PMIX_CLASS_DECLARATION(pmix_listener_t);

typedef struct {
    pmix_object_t super;
    prte_event_t ev;
    int fd;
    struct sockaddr_storage addr;
} prte_pending_connection_t;
PMIX_CLASS_DECLARATION(prte_pending_connection_t);

PRTE_EXPORT int prte_start_listening(void);
PRTE_EXPORT void prte_stop_listening(void);
PRTE_EXPORT int prte_register_listener(struct sockaddr *address, prte_socklen_t addrlen,
                                       prte_event_base_t *evbase,
                                       pmix_listener_callback_fn_t handler);

#endif /* PMIX_LISTENER_H */
