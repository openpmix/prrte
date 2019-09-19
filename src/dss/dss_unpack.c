/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2012-2015 Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2015 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "src/include/types.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/dss/dss_internal.h"

int prrte_dss_unpack(prrte_buffer_t *buffer, void *dst, int32_t *num_vals,
                    prrte_data_type_t type)
{
    int rc, ret;
    int32_t local_num, n=1;
    prrte_data_type_t local_type;

    /* check for error */
    if (NULL == buffer || NULL == dst || NULL == num_vals) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* if user provides a zero for num_vals, then there is no storage allocated
     * so return an appropriate error
     */
    if (0 == *num_vals) {
        PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack: inadequate space ( %p, %p, %lu, %d )\n",
                       (void*)buffer, dst, (long unsigned int)*num_vals, (int)type ) );
        return PRRTE_ERR_UNPACK_INADEQUATE_SPACE;
    }

    /** Unpack the declared number of values
     * REMINDER: it is possible that the buffer is corrupted and that
     * the DSS will *think* there is a proper int32_t variable at the
     * beginning of the unpack region - but that the value is bogus (e.g., just
     * a byte field in a string array that so happens to have a value that
     * matches the int32_t data type flag). Therefore, this error check is
     * NOT completely safe. This is true for ALL unpack functions, not just
     * int32_t as used here.
     */
    if (PRRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        if (PRRTE_SUCCESS != (
            rc = prrte_dss_get_data_type(buffer, &local_type))) {
            *num_vals = 0;
            return rc;
        }
        if (PRRTE_INT32 != local_type) { /* if the length wasn't first, then error */
            *num_vals = 0;
            return PRRTE_ERR_UNPACK_FAILURE;
        }
    }

    n=1;
    if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_int32(buffer, &local_num, &n, PRRTE_INT32))) {
        *num_vals = 0;
        return rc;
    }

    /** if the storage provided is inadequate, set things up
     * to unpack as much as we can and to return an error code
     * indicating that everything was not unpacked - the buffer
     * is left in a state where it can not be further unpacked.
     */
    if (local_num > *num_vals) {
        local_num = *num_vals;
        PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack: inadequate space ( %p, %p, %lu, %d )\n",
                       (void*)buffer, dst, (long unsigned int)*num_vals, (int)type ) );
        ret = PRRTE_ERR_UNPACK_INADEQUATE_SPACE;
    } else {  /** enough or more than enough storage */
        *num_vals = local_num;  /** let the user know how many we actually unpacked */
        ret = PRRTE_SUCCESS;
    }

    /** Unpack the value(s) */
    if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, dst, &local_num, type))) {
        *num_vals = 0;
        ret = rc;
    }

    return ret;
}

int prrte_dss_unpack_buffer(prrte_buffer_t *buffer, void *dst, int32_t *num_vals,
                    prrte_data_type_t type)
{
    int rc;
    prrte_data_type_t local_type;
    prrte_dss_type_info_t *info;

    PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack_buffer( %p, %p, %lu, %d )\n",
                   (void*)buffer, dst, (long unsigned int)*num_vals, (int)type ) );

    /** Unpack the declared data type */
    if (PRRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        if (PRRTE_SUCCESS != (rc = prrte_dss_get_data_type(buffer, &local_type))) {
            return rc;
        }
        /* if the data types don't match, then return an error */
        if (type != local_type) {
            prrte_output(0, "PRRTE dss:unpack: got type %d when expecting type %d", local_type, type);
            return PRRTE_ERR_PACK_MISMATCH;
        }
    }

    /* Lookup the unpack function for this type and call it */

    if (NULL == (info = (prrte_dss_type_info_t*)prrte_pointer_array_get_item(&prrte_dss_types, type))) {
        return PRRTE_ERR_UNPACK_FAILURE;
    }

    return info->odti_unpack_fn(buffer, dst, num_vals, type);
}


/* UNPACK GENERIC SYSTEM TYPES */

/*
 * BOOL
 */
int prrte_dss_unpack_bool(prrte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prrte_data_type_t type)
{
    int ret;
    prrte_data_type_t remote_type;

    if (PRRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        /* see what type was actually packed */
        if (PRRTE_SUCCESS != (ret = prrte_dss_peek_type(buffer, &remote_type))) {
            return ret;
        }
    } else {
        if (PRRTE_SUCCESS != (ret = prrte_dss_get_data_type(buffer, &remote_type))) {
            return ret;
        }
    }

    if (remote_type == DSS_TYPE_BOOL) {
        /* fast path it if the sizes are the same */
        /* Turn around and unpack the real type */
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, dest, num_vals, DSS_TYPE_BOOL))) {
        }
    } else {
        /* slow path - types are different sizes */
        UNPACK_SIZE_MISMATCH(bool, remote_type, ret);
    }
    return ret;
}

/*
 * INT
 */
int prrte_dss_unpack_int(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type)
{
    int ret;
    prrte_data_type_t remote_type;

    if (PRRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        /* see what type was actually packed */
        if (PRRTE_SUCCESS != (ret = prrte_dss_peek_type(buffer, &remote_type))) {
            return ret;
        }
    } else {
        if (PRRTE_SUCCESS != (ret = prrte_dss_get_data_type(buffer, &remote_type))) {
            return ret;
        }
    }

    if (remote_type == DSS_TYPE_INT) {
        /* fast path it if the sizes are the same */
        /* Turn around and unpack the real type */
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, dest, num_vals, DSS_TYPE_INT))) {
        }
    } else {
        /* slow path - types are different sizes */
        UNPACK_SIZE_MISMATCH(int, remote_type, ret);
    }

    return ret;
}

/*
 * SIZE_T
 */
int prrte_dss_unpack_sizet(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type)
{
    int ret;
    prrte_data_type_t remote_type;

    if (PRRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        /* see what type was actually packed */
        if (PRRTE_SUCCESS != (ret = prrte_dss_peek_type(buffer, &remote_type))) {
            return ret;
        }
    } else {
        if (PRRTE_SUCCESS != (ret = prrte_dss_get_data_type(buffer, &remote_type))) {
            return ret;
        }
    }

    if (remote_type == DSS_TYPE_SIZE_T) {
        /* fast path it if the sizes are the same */
        /* Turn around and unpack the real type */
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, dest, num_vals, DSS_TYPE_SIZE_T))) {
        }
    } else {
        /* slow path - types are different sizes */
        UNPACK_SIZE_MISMATCH(size_t, remote_type, ret);
    }

    return ret;
}

/*
 * PID_T
 */
int prrte_dss_unpack_pid(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type)
{
    int ret;
    prrte_data_type_t remote_type;

    if (PRRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        /* see what type was actually packed */
        if (PRRTE_SUCCESS != (ret = prrte_dss_peek_type(buffer, &remote_type))) {
            return ret;
        }
    } else {
        if (PRRTE_SUCCESS != (ret = prrte_dss_get_data_type(buffer, &remote_type))) {
            return ret;
        }
    }

    if (remote_type == DSS_TYPE_PID_T) {
        /* fast path it if the sizes are the same */
        /* Turn around and unpack the real type */
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, dest, num_vals, DSS_TYPE_PID_T))) {
        }
    } else {
        /* slow path - types are different sizes */
        UNPACK_SIZE_MISMATCH(pid_t, remote_type, ret);
    }

    return ret;
}


/* UNPACK FUNCTIONS FOR NON-GENERIC SYSTEM TYPES */

/*
 * NULL
 */
int prrte_dss_unpack_null(prrte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prrte_data_type_t type)
{
    PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack_null * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prrte_dss_too_small(buffer, *num_vals)) {
        return PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    memcpy(dest, buffer->unpack_ptr, *num_vals);

    /* update buffer pointer */
    buffer->unpack_ptr += *num_vals;

    return PRRTE_SUCCESS;
}

/*
 * BYTE, CHAR, INT8
 */
int prrte_dss_unpack_byte(prrte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prrte_data_type_t type)
{
    PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack_byte * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prrte_dss_too_small(buffer, *num_vals)) {
        return PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    memcpy(dest, buffer->unpack_ptr, *num_vals);

    /* update buffer pointer */
    buffer->unpack_ptr += *num_vals;

    return PRRTE_SUCCESS;
}

int prrte_dss_unpack_int16(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type)
{
    int32_t i;
    uint16_t tmp, *desttmp = (uint16_t*) dest;

   PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack_int16 * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prrte_dss_too_small(buffer, (*num_vals)*sizeof(tmp))) {
        return PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        memcpy( &(tmp), buffer->unpack_ptr, sizeof(tmp) );
        tmp = ntohs(tmp);
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
        buffer->unpack_ptr += sizeof(tmp);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_unpack_int32(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type)
{
    int32_t i;
    uint32_t tmp, *desttmp = (uint32_t*) dest;

   PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack_int32 * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prrte_dss_too_small(buffer, (*num_vals)*sizeof(tmp))) {
        return PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        memcpy( &(tmp), buffer->unpack_ptr, sizeof(tmp) );
        tmp = ntohl(tmp);
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
        buffer->unpack_ptr += sizeof(tmp);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_unpack_int64(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type)
{
    int32_t i;
    uint64_t tmp, *desttmp = (uint64_t*) dest;

   PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack_int64 * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prrte_dss_too_small(buffer, (*num_vals)*sizeof(tmp))) {
        return PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        memcpy( &(tmp), buffer->unpack_ptr, sizeof(tmp) );
        tmp = prrte_ntoh64(tmp);
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
        buffer->unpack_ptr += sizeof(tmp);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_unpack_string(prrte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prrte_data_type_t type)
{
    int ret;
    int32_t i, len, n=1;
    char **sdest = (char**) dest;

    for (i = 0; i < (*num_vals); ++i) {
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_int32(buffer, &len, &n, PRRTE_INT32))) {
            return ret;
        }
        if (0 ==  len) {   /* zero-length string - unpack the NULL */
            sdest[i] = NULL;
        } else {
        sdest[i] = (char*)malloc(len);
            if (NULL == sdest[i]) {
                return PRRTE_ERR_OUT_OF_RESOURCE;
            }
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_byte(buffer, sdest[i], &len, PRRTE_BYTE))) {
                return ret;
            }
        }
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_unpack_float(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type)
{
    int32_t i, n;
    float *desttmp = (float*) dest, tmp;
    int ret;
    char *convert;

   PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack_float * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prrte_dss_too_small(buffer, (*num_vals)*sizeof(float))) {
        return PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        n=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_string(buffer, &convert, &n, PRRTE_STRING))) {
            return ret;
        }
        if (NULL == convert) {
            return PRRTE_ERR_UNPACK_FAILURE;
        }
        tmp = strtof(convert, NULL);
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
        free(convert);
        convert = NULL;
    }
    return PRRTE_SUCCESS;
}

int prrte_dss_unpack_double(prrte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prrte_data_type_t type)
{
    int32_t i, n;
    double *desttmp = (double*) dest, tmp;
    int ret;
    char *convert;

   PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack_double * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prrte_dss_too_small(buffer, (*num_vals)*sizeof(double))) {
        return PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        n=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_string(buffer, &convert, &n, PRRTE_STRING))) {
            return ret;
        }
        if (NULL == convert) {
            return PRRTE_ERR_UNPACK_FAILURE;
        }
        tmp = strtod(convert, NULL);
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
        free(convert);
        convert = NULL;
    }
    return PRRTE_SUCCESS;
}

int prrte_dss_unpack_timeval(prrte_buffer_t *buffer, void *dest,
                            int32_t *num_vals, prrte_data_type_t type)
{
    int32_t i, n;
    int64_t tmp[2];
    struct timeval *desttmp = (struct timeval *) dest, tt;
    int ret;

   PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack_timeval * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prrte_dss_too_small(buffer, (*num_vals)*sizeof(struct timeval))) {
        return PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        n=2;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_int64(buffer, tmp, &n, PRRTE_INT64))) {
            return ret;
        }
        tt.tv_sec = tmp[0];
        tt.tv_usec = tmp[1];
        memcpy(&desttmp[i], &tt, sizeof(tt));
    }
    return PRRTE_SUCCESS;
}

int prrte_dss_unpack_time(prrte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prrte_data_type_t type)
{
    int32_t i, n;
    time_t *desttmp = (time_t *) dest, tmp;
    int ret;
    uint64_t ui64;

    /* time_t is a system-dependent size, so cast it
     * to uint64_t as a generic safe size
     */

   PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_unpack_time * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
   if (prrte_dss_too_small(buffer, (*num_vals)*(sizeof(uint64_t)))) {
        return PRRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        n=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_int64(buffer, &ui64, &n, PRRTE_UINT64))) {
            return ret;
        }
        tmp = (time_t)ui64;
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
    }
    return PRRTE_SUCCESS;
}


/* UNPACK FUNCTIONS FOR GENERIC PRRTE TYPES */

/*
 * PRRTE_DATA_TYPE
 */
int prrte_dss_unpack_data_type(prrte_buffer_t *buffer, void *dest, int32_t *num_vals,
                             prrte_data_type_t type)
{
     /* turn around and unpack the real type */
    return prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_DATA_TYPE_T);
}

/*
 * PRRTE_BYTE_OBJECT
 */
int prrte_dss_unpack_byte_object(prrte_buffer_t *buffer, void *dest, int32_t *num,
                             prrte_data_type_t type)
{
    int ret;
    int32_t i, n, m=1;
    prrte_byte_object_t **dbyteptr;

    dbyteptr = (prrte_byte_object_t**)dest;
    n = *num;
    for(i=0; i<n; i++) {
        /* allocate memory for the byte object itself */
        dbyteptr[i] = (prrte_byte_object_t*)malloc(sizeof(prrte_byte_object_t));
        if (NULL == dbyteptr[i]) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        /* unpack object size in bytes */
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_int32(buffer, &(dbyteptr[i]->size), &m, PRRTE_INT32))) {
            return ret;
        }
        if (0 < dbyteptr[i]->size) {
            dbyteptr[i]->bytes = (uint8_t*)malloc(dbyteptr[i]->size);
            if (NULL == dbyteptr[i]->bytes) {
                return PRRTE_ERR_OUT_OF_RESOURCE;
            }
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_byte(buffer, (dbyteptr[i]->bytes),
                                            &(dbyteptr[i]->size), PRRTE_BYTE))) {
                return ret;
            }
        } else {
            /* be sure to init the bytes pointer to NULL! */
            dbyteptr[i]->bytes = NULL;
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * PRRTE_PSTAT
 */
int prrte_dss_unpack_pstat(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type)
{
    prrte_pstats_t **ptr;
    int32_t i, n, m;
    int ret;
    char *cptr;

    ptr = (prrte_pstats_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRRTE_NEW(prrte_pstats_t);
        if (NULL == ptr[i]) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &cptr, &m, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        memmove(ptr[i]->node, cptr, strlen(cptr));
        free(cptr);
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->rank, &m, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->pid, &m, PRRTE_PID))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &cptr, &m, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        memmove(ptr[i]->cmd, cptr, strlen(cptr));
        free(cptr);
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->state[0], &m, PRRTE_BYTE))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->time, &m, PRRTE_TIMEVAL))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->priority, &m, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->num_threads, &m, PRRTE_INT16))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->pss, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->vsize, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->rss, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->peak_vsize, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->processor, &m, PRRTE_INT16))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->sample_time, &m, PRRTE_TIMEVAL))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
    }

    return PRRTE_SUCCESS;
}

static int unpack_disk_stats(prrte_buffer_t *buffer, prrte_node_stats_t *ns)
{
    int32_t i, m, n;
    int ret;
    prrte_diskstats_t *dk;
    uint64_t i64;

    /* unpack the number of disk stat objects */
    m=1;
    if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &n, &m, PRRTE_INT32))) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }
    /* unpack them */
    for (i=0; i < n; i++) {
        dk = PRRTE_NEW(prrte_diskstats_t);
        assert(dk);
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &dk->disk, &m, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        dk->num_reads_completed = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        dk->num_reads_merged = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        dk->num_sectors_read = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        dk->milliseconds_reading = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        dk->num_writes_completed = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        dk->num_writes_merged = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        dk->num_sectors_written = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        dk->milliseconds_writing = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        dk->num_ios_in_progress = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        dk->milliseconds_io = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(dk);
            return ret;
        }
        dk->weighted_milliseconds_io = i64;
        prrte_list_append(&ns->diskstats, &dk->super);
    }
    return PRRTE_SUCCESS;
}

static int unpack_net_stats(prrte_buffer_t *buffer, prrte_node_stats_t *ns)
{
    int32_t i, m, n;
    int ret;
    prrte_netstats_t *net;
    uint64_t i64;

    /* unpack the number of net stat objects */
    m=1;
    if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &n, &m, PRRTE_INT32))) {
        PRRTE_ERROR_LOG(ret);
        return ret;
    }
    /* unpack them */
    for (i=0; i < n; i++) {
        net = PRRTE_NEW(prrte_netstats_t);
        assert(net);
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &net->net_interface, &m, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(net);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(net);
            return ret;
        }
        net->num_bytes_recvd = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(net);
            return ret;
        }
        net->num_packets_recvd = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(net);
            return ret;
        }
        net->num_recv_errs = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(net);
            return ret;
        }
        net->num_bytes_sent = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(net);
            return ret;
        }
        net->num_packets_sent = i64;
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &i64, &m, PRRTE_UINT64))) {
            PRRTE_ERROR_LOG(ret);
            PRRTE_RELEASE(net);
            return ret;
        }
        net->num_send_errs = i64;
        prrte_list_append(&ns->netstats, &net->super);
    }
    return PRRTE_SUCCESS;
}

/*
 * PRRTE_NODE_STAT
 */
int prrte_dss_unpack_node_stat(prrte_buffer_t *buffer, void *dest,
                              int32_t *num_vals, prrte_data_type_t type)
{
    prrte_node_stats_t **ptr;
    int32_t i, n, m;
    int ret;

    ptr = (prrte_node_stats_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRRTE_NEW(prrte_node_stats_t);
        if (NULL == ptr[i]) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->la, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->la5, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->la15, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->total_mem, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->free_mem, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->buffers, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->cached, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->swap_cached, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->swap_total, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->swap_free, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_float(buffer, &ptr[i]->mapped, &m, PRRTE_FLOAT))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->sample_time, &m, PRRTE_TIMEVAL))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        /* unpack the disk stat objects */
        if (PRRTE_SUCCESS != (ret = unpack_disk_stats(buffer, ptr[i]))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        /* unpack the net stat objects */
        if (PRRTE_SUCCESS != (ret = unpack_net_stats(buffer, ptr[i]))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        PRRTE_RELEASE(ptr[i]);
    }

    return PRRTE_SUCCESS;
}

/*
 * PRRTE_VALUE
 */
int prrte_dss_unpack_value(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type)
{
    prrte_value_t **ptr;
    int32_t i, n, m;
    int ret;

    ptr = (prrte_value_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRRTE_NEW(prrte_value_t);
        if (NULL == ptr[i]) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        /* unpack the key and type */
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_string(buffer, &ptr[i]->key, &m, PRRTE_STRING))) {
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_data_type(buffer, &ptr[i]->type, &m, PRRTE_DATA_TYPE))) {
            return ret;
        }
        /* now unpack the right field */
        m=1;
        switch (ptr[i]->type) {
        case PRRTE_BOOL:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.flag, &m, PRRTE_BOOL))) {
                return ret;
            }
            break;
        case PRRTE_BYTE:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.byte, &m, PRRTE_BYTE))) {
                return ret;
            }
            break;
        case PRRTE_STRING:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.string, &m, PRRTE_STRING))) {
                return ret;
            }
            break;
        case PRRTE_SIZE:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.size, &m, PRRTE_SIZE))) {
                return ret;
            }
            break;
        case PRRTE_PID:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.pid, &m, PRRTE_PID))) {
                return ret;
            }
            break;
        case PRRTE_INT:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.integer, &m, PRRTE_INT))) {
                return ret;
            }
            break;
        case PRRTE_INT8:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.int8, &m, PRRTE_INT8))) {
                return ret;
            }
            break;
        case PRRTE_INT16:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.int16, &m, PRRTE_INT16))) {
                return ret;
            }
            break;
        case PRRTE_INT32:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.int32, &m, PRRTE_INT32))) {
                return ret;
            }
            break;
        case PRRTE_INT64:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.int64, &m, PRRTE_INT64))) {
                return ret;
            }
            break;
        case PRRTE_UINT:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.uint, &m, PRRTE_UINT))) {
                return ret;
            }
            break;
        case PRRTE_UINT8:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.uint8, &m, PRRTE_UINT8))) {
                return ret;
            }
            break;
        case PRRTE_UINT16:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.uint16, &m, PRRTE_UINT16))) {
                return ret;
            }
            break;
        case PRRTE_UINT32:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.uint32, &m, PRRTE_UINT32))) {
                return ret;
            }
            break;
        case PRRTE_UINT64:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.uint64, &m, PRRTE_UINT64))) {
                return ret;
            }
            break;
        case PRRTE_BYTE_OBJECT:
            /* cannot use byte object unpack as it allocates memory, so unpack object size in bytes */
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_int32(buffer, &(ptr[i]->data.bo.size), &m, PRRTE_INT32))) {
                return ret;
            }
            if (0 < ptr[i]->data.bo.size) {
                ptr[i]->data.bo.bytes = (uint8_t*)malloc(ptr[i]->data.bo.size);
                if (NULL == ptr[i]->data.bo.bytes) {
                    return PRRTE_ERR_OUT_OF_RESOURCE;
                }
                if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_byte(buffer, ptr[i]->data.bo.bytes,
                                                                &(ptr[i]->data.bo.size), PRRTE_BYTE))) {
                    return ret;
                }
            } else {
                ptr[i]->data.bo.bytes = NULL;
            }
            break;
        case PRRTE_FLOAT:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.fval, &m, PRRTE_FLOAT))) {
                return ret;
            }
            break;
        case PRRTE_DOUBLE:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.dval, &m, PRRTE_DOUBLE))) {
                return ret;
            }
            break;
        case PRRTE_TIMEVAL:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.tv, &m, PRRTE_TIMEVAL))) {
                return ret;
            }
            break;
        case PRRTE_TIME:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.time, &m, PRRTE_TIME))) {
                return ret;
            }
            break;
        case PRRTE_PTR:
            /* just ignore these values */
            break;
        case PRRTE_NAME:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.name, &m, PRRTE_NAME))) {
                return ret;
            }
            break;
        case PRRTE_STATUS:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.status, &m, PRRTE_INT))) {
                return ret;
            }
            break;
        case PRRTE_ENVAR:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.envar, &m, PRRTE_ENVAR))) {
                return ret;
            }
            break;
        default:
            prrte_output(0, "UNPACK-PRRTE-VALUE: UNSUPPORTED TYPE %d FOR KEY %s", (int)ptr[i]->type, ptr[i]->key);
            return PRRTE_ERROR;
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * PRRTE_BUFFER
 */
int prrte_dss_unpack_buffer_contents(prrte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prrte_data_type_t type)
{
    prrte_buffer_t **ptr;
    int32_t i, n, m;
    int ret;
    size_t nbytes;

    ptr = (prrte_buffer_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRRTE_NEW(prrte_buffer_t);
        if (NULL == ptr[i]) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        /* unpack the number of bytes */
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_sizet(buffer, &nbytes, &m, PRRTE_SIZE))) {
            return ret;
        }
        m = nbytes;
        /* setup the buffer's data region */
        if (0 < nbytes) {
            ptr[i]->base_ptr = (char*)malloc(nbytes);
            /* unpack the bytes */
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_byte(buffer, ptr[i]->base_ptr, &m, PRRTE_BYTE))) {
                return ret;
            }
        }
        ptr[i]->pack_ptr = ptr[i]->base_ptr + m;
        ptr[i]->unpack_ptr = ptr[i]->base_ptr;
        ptr[i]->bytes_allocated = nbytes;
        ptr[i]->bytes_used = m;
    }
    return PRRTE_SUCCESS;
}

/*
 * NAME
 */
int prrte_dss_unpack_name(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type)
{
    int rc;
    int32_t i, num;
    prrte_process_name_t* proc;
    prrte_jobid_t *jobid;
    prrte_vpid_t *vpid;

    num = *num_vals;

    /* allocate space for all the jobids in a contiguous array */
    jobid = (prrte_jobid_t*)malloc(num * sizeof(prrte_jobid_t));
    if (NULL == jobid) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        *num_vals = 0;
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    /* now unpack them in one shot */
    if (PRRTE_SUCCESS != (rc =
                         prrte_dss_unpack_jobid(buffer, jobid, num_vals, PRRTE_JOBID))) {
        PRRTE_ERROR_LOG(rc);
        *num_vals = 0;
        free(jobid);
        return rc;
    }

    /* collect all the vpids in a contiguous array */
    vpid = (prrte_vpid_t*)malloc(num * sizeof(prrte_vpid_t));
    if (NULL == vpid) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        *num_vals = 0;
        free(jobid);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    /* now unpack them in one shot */
    if (PRRTE_SUCCESS != (rc =
                         prrte_dss_unpack_vpid(buffer, vpid, num_vals, PRRTE_VPID))) {
        PRRTE_ERROR_LOG(rc);
        *num_vals = 0;
        free(vpid);
        free(jobid);
        return rc;
    }

    /* build the names from the jobid/vpid arrays */
    proc = (prrte_process_name_t*)dest;
    for (i=0; i < num; i++) {
        proc->jobid = jobid[i];
        proc->vpid = vpid[i];
        proc++;
    }

    /* cleanup */
    free(vpid);
    free(jobid);

    return PRRTE_SUCCESS;
}

/*
 * JOBID
 */
int prrte_dss_unpack_jobid(prrte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prrte_data_type_t type)
{
    int ret;

    /* Turn around and unpack the real type */
    if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_JOBID_T))) {
        PRRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * VPID
 */
int prrte_dss_unpack_vpid(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type)
{
    int ret;

    /* Turn around and unpack the real type */
    if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_VPID_T))) {
        PRRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * STATUS
 */
int prrte_dss_unpack_status(prrte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prrte_data_type_t type)
{
    int ret;

    /* Turn around and unpack the real type */
    ret = prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_INT);
    if (PRRTE_SUCCESS != ret) {
        PRRTE_ERROR_LOG(ret);
    }

    return ret;
}


int prrte_dss_unpack_envar(prrte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prrte_data_type_t type)
{
    prrte_envar_t *ptr;
    int32_t i, n, m;
    int ret;

    ptr = (prrte_envar_t *) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_string(buffer, &ptr[i].envar, &m, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_string(buffer, &ptr[i].value, &m, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_byte(buffer, &ptr[i].separator, &m, PRRTE_BYTE))) {
            PRRTE_ERROR_LOG(ret);
            return ret;
        }
    }

    return PRRTE_SUCCESS;
}
