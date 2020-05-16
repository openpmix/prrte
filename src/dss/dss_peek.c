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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/dss/dss_internal.h"

int prte_dss_peek(prte_buffer_t *buffer, prte_data_type_t *type,
                  int32_t *num_vals)
{
    int ret;
    prte_buffer_t tmp;
    int32_t n=1;
    prte_data_type_t local_type;

    /* check for errors */
    if (buffer == NULL) {
        return PRTE_ERR_BAD_PARAM;
    }

    /* Double check and ensure that there is data left in the buffer. */

    if (buffer->unpack_ptr >= buffer->base_ptr + buffer->bytes_used) {
        *type = PRTE_NULL;
        *num_vals = 0;
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* if this is NOT a fully described buffer, then that is as much as
     * we can do - there is no way we can tell the caller what type is
     * in the buffer since that info wasn't stored.
     */
    if (PRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        *type = PRTE_UNDEF;
        *num_vals = 0;
        return PRTE_ERR_UNKNOWN_DATA_TYPE;
    }

    /* cheat: unpack from a copy of the buffer -- leaving all the
       original pointers intact */
    tmp = *buffer;

    if (PRTE_SUCCESS != (ret = prte_dss_get_data_type(&tmp, &local_type))) {
        *type = PRTE_NULL;
        *num_vals = 0;
        return ret;
    }
    if (PRTE_INT32 != local_type) { /* if the length wasn't first, then error */
        *type = PRTE_NULL;
        *num_vals = 0;
        return PRTE_ERR_UNPACK_FAILURE;
    }
    if (PRTE_SUCCESS != (ret = prte_dss_unpack_int32(&tmp, num_vals, &n, PRTE_INT32))) {
        *type = PRTE_NULL;
        *num_vals = 0;
        return ret;
    }
    if (PRTE_SUCCESS != (ret = prte_dss_get_data_type(&tmp, type))) {
        *type = PRTE_NULL;
        *num_vals = 0;
    }

    return ret;
}

int prte_dss_peek_type(prte_buffer_t *buffer, prte_data_type_t *type)
{
    int ret;
    prte_buffer_t tmp;

    /* check for errors */
    if (buffer == NULL) {
        return PRTE_ERR_BAD_PARAM;
    }

    /* if this is NOT a fully described buffer, then there isn't anything
     * we can do - there is no way we can tell the caller what type is
     * in the buffer since that info wasn't stored.
     */
    if (PRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        *type = PRTE_UNDEF;
        return PRTE_ERR_UNKNOWN_DATA_TYPE;
    }
    /* Double check and ensure that there is data left in the buffer. */

    if (buffer->unpack_ptr >= buffer->base_ptr + buffer->bytes_used) {
        *type = PRTE_UNDEF;
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* cheat: unpack from a copy of the buffer -- leaving all the
    original pointers intact */
    tmp = *buffer;

    if (PRTE_SUCCESS != (ret = prte_dss_get_data_type(&tmp, type))) {
        *type = PRTE_UNDEF;
        return ret;
    }

    return PRTE_SUCCESS;
}
