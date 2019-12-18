/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/dss/dss_internal.h"

int prrte_dss_peek(prrte_buffer_t *buffer, prrte_data_type_t *type,
                  int32_t *num_vals)
{
    int ret;
    prrte_buffer_t tmp;
    int32_t n=1;
    prrte_data_type_t local_type;

    /* check for errors */
    if (buffer == NULL) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* Double check and ensure that there is data left in the buffer. */

    if (buffer->unpack_ptr >= buffer->base_ptr + buffer->bytes_used) {
        *type = PRRTE_NULL;
        *num_vals = 0;
        return PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* if this is NOT a fully described buffer, then that is as much as
     * we can do - there is no way we can tell the caller what type is
     * in the buffer since that info wasn't stored.
     */
    if (PRRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        *type = PRRTE_UNDEF;
        *num_vals = 0;
        return PRRTE_ERR_UNKNOWN_DATA_TYPE;
    }

    /* cheat: unpack from a copy of the buffer -- leaving all the
       original pointers intact */
    tmp = *buffer;

    if (PRRTE_SUCCESS != (ret = prrte_dss_get_data_type(&tmp, &local_type))) {
        *type = PRRTE_NULL;
        *num_vals = 0;
        return ret;
    }
    if (PRRTE_INT32 != local_type) { /* if the length wasn't first, then error */
        *type = PRRTE_NULL;
        *num_vals = 0;
        return PRRTE_ERR_UNPACK_FAILURE;
    }
    if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_int32(&tmp, num_vals, &n, PRRTE_INT32))) {
        *type = PRRTE_NULL;
        *num_vals = 0;
        return ret;
    }
    if (PRRTE_SUCCESS != (ret = prrte_dss_get_data_type(&tmp, type))) {
        *type = PRRTE_NULL;
        *num_vals = 0;
    }

    return ret;
}

int prrte_dss_peek_type(prrte_buffer_t *buffer, prrte_data_type_t *type)
{
    int ret;
    prrte_buffer_t tmp;

    /* check for errors */
    if (buffer == NULL) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* if this is NOT a fully described buffer, then there isn't anything
     * we can do - there is no way we can tell the caller what type is
     * in the buffer since that info wasn't stored.
     */
    if (PRRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        *type = PRRTE_UNDEF;
        return PRRTE_ERR_UNKNOWN_DATA_TYPE;
    }
    /* Double check and ensure that there is data left in the buffer. */

    if (buffer->unpack_ptr >= buffer->base_ptr + buffer->bytes_used) {
        *type = PRRTE_UNDEF;
        return PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* cheat: unpack from a copy of the buffer -- leaving all the
    original pointers intact */
    tmp = *buffer;

    if (PRRTE_SUCCESS != (ret = prrte_dss_get_data_type(&tmp, type))) {
        *type = PRRTE_UNDEF;
        return ret;
    }

    return PRRTE_SUCCESS;
}
