/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * Data server for PRTE
 */
#ifndef PRTE_DS_INTERNAL_H
#define PRTE_DS_INTERNAL_H

#include "prte_config.h"
#include "types.h"

#include "src/pmix/pmix-internal.h"

BEGIN_C_DECLS

/* define an object to hold data */
typedef struct {
    /* base object */
    pmix_object_t super;
    /* index of this object in the storage array */
    int32_t index;
    // daemon that sent the data
    pmix_proc_t proxy;
    /* process that owns this data - only the
     * owner can remove it
     */
    pmix_proc_t owner;
    /* uid of the owner - helps control
     * access rights */
    uint32_t uid;
    /* characteristics */
    pmix_data_range_t range;
    pmix_persistence_t persistence;
    /* and the values themselves - we store them as a list
     * because we may (if persistence is set to "first-read")
     * remove them upon read */
    pmix_list_t info;
} prte_data_object_t;
PMIX_CLASS_DECLARATION(prte_data_object_t);


/* define a request object for delayed answers */
typedef struct {
    pmix_list_item_t super;
    pmix_proc_t proxy;
    pmix_proc_t requestor;
    int room_number;
    uint32_t uid;
    pmix_data_range_t range;
    char **keys;
} prte_data_req_t;
PMIX_CLASS_DECLARATION(prte_data_req_t);


/* define a container for data object cleanups */
typedef struct {
    pmix_list_item_t super;
    prte_data_object_t *data;
} prte_data_cleanup_t;


/* define a caddy for pointing to pmix_info_t that
 * are to be included in an answer */
typedef struct {
    pmix_list_item_t super;
    pmix_proc_t source;
    pmix_info_t info;
} prte_ds_info_t;
PMIX_CLASS_DECLARATION(prte_ds_info_t);


typedef struct {
    pmix_pointer_array_t store;
    pmix_list_t pending;
    int output;
    int verbosity;
} prte_data_store_t;

extern prte_data_store_t prte_data_store;

PRTE_EXPORT pmix_status_t prte_ds_publish(pmix_proc_t *sender,
                                          pmix_data_buffer_t *buffer,
                                          pmix_data_buffer_t *answer);

PRTE_EXPORT pmix_status_t prte_ds_lookup(pmix_proc_t *sender, int room_number,
                                         pmix_data_buffer_t *buffer,
                                         pmix_data_buffer_t *answer);

PRTE_EXPORT pmix_status_t prte_ds_unpublish(pmix_proc_t *sender,
                                            pmix_data_buffer_t *buffer,
                                            pmix_data_buffer_t *answer);

PRTE_EXPORT void prte_ds_purge(pmix_proc_t *sender,
                               pmix_data_buffer_t *buffer,
                               pmix_data_buffer_t *answer);

PRTE_EXPORT pmix_status_t prte_data_server_check_range(prte_data_req_t *req,
                                                       prte_data_object_t *data);

END_C_DECLS

#endif /* PRTE_DS_INTERNAL_H */
