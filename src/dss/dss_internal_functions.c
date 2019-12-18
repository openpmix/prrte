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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/class/prrte_pointer_array.h"

#include "src/dss/dss_internal.h"

/**
 * Internal function that resizes (expands) an inuse buffer if
 * necessary.
 */
char* prrte_dss_buffer_extend(prrte_buffer_t *buffer, size_t bytes_to_add)
{
    size_t required, to_alloc;
    size_t pack_offset, unpack_offset;

    /* Check to see if we have enough space already */
    if ((buffer->bytes_allocated - buffer->bytes_used) >= bytes_to_add) {
        return buffer->pack_ptr;
    }

    required = buffer->bytes_used + bytes_to_add;
    if (required >= (size_t)prrte_dss_threshold_size) {
        to_alloc = ((required + prrte_dss_threshold_size - 1)
                    / prrte_dss_threshold_size) * prrte_dss_threshold_size;
    } else {
        to_alloc = buffer->bytes_allocated;
        if(0 == to_alloc) {
            to_alloc = prrte_dss_initial_size;
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
bool prrte_dss_too_small(prrte_buffer_t *buffer, size_t bytes_reqd)
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

int prrte_dss_store_data_type(prrte_buffer_t *buffer, prrte_data_type_t type)
{
    prrte_dss_type_info_t *info;

    /* Lookup the pack function for the actual prrte_data_type type and call it */

    if (NULL == (info = (prrte_dss_type_info_t*)prrte_pointer_array_get_item(&prrte_dss_types, PRRTE_DATA_TYPE_T))) {
        return PRRTE_ERR_PACK_FAILURE;
    }

    return info->odti_pack_fn(buffer, &type, 1, PRRTE_DATA_TYPE_T);
}

int prrte_dss_get_data_type(prrte_buffer_t *buffer, prrte_data_type_t *type)
{
    prrte_dss_type_info_t *info;
    int32_t n=1;

    /* Lookup the unpack function for the actual prrte_data_type type and call it */

    if (NULL == (info = (prrte_dss_type_info_t*)prrte_pointer_array_get_item(&prrte_dss_types, PRRTE_DATA_TYPE_T))) {
        return PRRTE_ERR_PACK_FAILURE;
    }

    return info->odti_unpack_fn(buffer, type, &n, PRRTE_DATA_TYPE_T);
}
