/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/class/prte_pointer_array.h"

#include "src/dss/dss_internal.h"

/**
 * Internal function that resizes (expands) an inuse buffer if
 * necessary.
 */
char* prte_dss_buffer_extend(prte_buffer_t *buffer, size_t bytes_to_add)
{
    size_t required, to_alloc;
    size_t pack_offset, unpack_offset;

    /* Check to see if we have enough space already */
    if ((buffer->bytes_allocated - buffer->bytes_used) >= bytes_to_add) {
        return buffer->pack_ptr;
    }

    required = buffer->bytes_used + bytes_to_add;
    if (required >= (size_t)prte_dss_threshold_size) {
        to_alloc = ((required + prte_dss_threshold_size - 1)
                    / prte_dss_threshold_size) * prte_dss_threshold_size;
    } else {
        to_alloc = buffer->bytes_allocated;
        if(0 == to_alloc) {
            to_alloc = prte_dss_initial_size;
        }
        while(to_alloc < required) {
            to_alloc <<= 1;
        }
    }

    if (NULL != buffer->base_ptr) {
        pack_offset = ((char*) buffer->pack_ptr) - ((char*) buffer->base_ptr);
        unpack_offset = ((char*) buffer->unpack_ptr) -
            ((char*) buffer->base_ptr);
        buffer->base_ptr = (char*)realloc(buffer->base_ptr, to_alloc);
    } else {
        pack_offset = 0;
        unpack_offset = 0;
        buffer->bytes_used = 0;
        buffer->base_ptr = (char*)malloc(to_alloc);
    }

    if (NULL == buffer->base_ptr) {
        return NULL;
    }
    buffer->pack_ptr = ((char*) buffer->base_ptr) + pack_offset;
    buffer->unpack_ptr = ((char*) buffer->base_ptr) + unpack_offset;
    buffer->bytes_allocated = to_alloc;

    /* All done */

    return buffer->pack_ptr;
}

/*
 * Internal function that checks to see if the specified number of bytes
 * remain in the buffer for unpacking
 */
bool prte_dss_too_small(prte_buffer_t *buffer, size_t bytes_reqd)
{
    size_t bytes_remaining_packed;

    if (buffer->pack_ptr < buffer->unpack_ptr) {
        return true;
    }

    bytes_remaining_packed = buffer->pack_ptr - buffer->unpack_ptr;

    if (bytes_remaining_packed < bytes_reqd) {
        /* don't error log this - it could be that someone is trying to
         * simply read until the buffer is empty
         */
        return true;
    }

    return false;
}

int prte_dss_store_data_type(prte_buffer_t *buffer, prte_data_type_t type)
{
    prte_dss_type_info_t *info;

    /* Lookup the pack function for the actual prte_data_type type and call it */

    if (NULL == (info = (prte_dss_type_info_t*)prte_pointer_array_get_item(&prte_dss_types, PRTE_DATA_TYPE_T))) {
        return PRTE_ERR_PACK_FAILURE;
    }

    return info->odti_pack_fn(buffer, &type, 1, PRTE_DATA_TYPE_T);
}

int prte_dss_get_data_type(prte_buffer_t *buffer, prte_data_type_t *type)
{
    prte_dss_type_info_t *info;
    int32_t n=1;

    /* Lookup the unpack function for the actual prte_data_type type and call it */

    if (NULL == (info = (prte_dss_type_info_t*)prte_pointer_array_get_item(&prte_dss_types, PRTE_DATA_TYPE_T))) {
        return PRTE_ERR_PACK_FAILURE;
    }

    return info->odti_unpack_fn(buffer, type, &n, PRTE_DATA_TYPE_T);
}
