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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "src/include/types.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/dss/dss_internal.h"

int prte_dss_unpack(prte_buffer_t *buffer, void *dst, int32_t *num_vals,
                    prte_data_type_t type)
{
    int rc, ret;
    int32_t local_num, n=1;
    prte_data_type_t local_type;

    /* check for error */
    if (NULL == buffer || NULL == dst || NULL == num_vals) {
        return PRTE_ERR_BAD_PARAM;
    }

    /* if user provides a zero for num_vals, then there is no storage allocated
     * so return an appropriate error
     */
    if (0 == *num_vals) {
        PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack: inadequate space ( %p, %p, %lu, %d )\n",
                       (void*)buffer, dst, (long unsigned int)*num_vals, (int)type ) );
        return PRTE_ERR_UNPACK_INADEQUATE_SPACE;
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
    if (PRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        if (PRTE_SUCCESS != (
            rc = prte_dss_get_data_type(buffer, &local_type))) {
            *num_vals = 0;
            return rc;
        }
        if (PRTE_INT32 != local_type) { /* if the length wasn't first, then error */
            *num_vals = 0;
            return PRTE_ERR_UNPACK_FAILURE;
        }
    }

    n=1;
    if (PRTE_SUCCESS != (rc = prte_dss_unpack_int32(buffer, &local_num, &n, PRTE_INT32))) {
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
        PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack: inadequate space ( %p, %p, %lu, %d )\n",
                       (void*)buffer, dst, (long unsigned int)*num_vals, (int)type ) );
        ret = PRTE_ERR_UNPACK_INADEQUATE_SPACE;
    } else {  /** enough or more than enough storage */
        *num_vals = local_num;  /** let the user know how many we actually unpacked */
        ret = PRTE_SUCCESS;
    }

    /** Unpack the value(s) */
    if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, dst, &local_num, type))) {
        *num_vals = 0;
        ret = rc;
    }

    return ret;
}

int prte_dss_unpack_buffer(prte_buffer_t *buffer, void *dst, int32_t *num_vals,
                    prte_data_type_t type)
{
    int rc;
    prte_data_type_t local_type;
    prte_dss_type_info_t *info;

    PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack_buffer( %p, %p, %lu, %d )\n",
                   (void*)buffer, dst, (long unsigned int)*num_vals, (int)type ) );

    /** Unpack the declared data type */
    if (PRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        if (PRTE_SUCCESS != (rc = prte_dss_get_data_type(buffer, &local_type))) {
            return rc;
        }
        /* if the data types don't match, then return an error */
        if (type != local_type) {
            prte_output(0, "PRTE dss:unpack: got type %d when expecting type %d", local_type, type);
            return PRTE_ERR_PACK_MISMATCH;
        }
    }

    /* Lookup the unpack function for this type and call it */

    if (NULL == (info = (prte_dss_type_info_t*)prte_pointer_array_get_item(&prte_dss_types, type))) {
        return PRTE_ERR_UNPACK_FAILURE;
    }

    return info->odti_unpack_fn(buffer, dst, num_vals, type);
}


/* UNPACK GENERIC SYSTEM TYPES */

/*
 * BOOL
 */
int prte_dss_unpack_bool(prte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prte_data_type_t type)
{
    int ret;
    prte_data_type_t remote_type;

    if (PRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        /* see what type was actually packed */
        if (PRTE_SUCCESS != (ret = prte_dss_peek_type(buffer, &remote_type))) {
            return ret;
        }
    } else {
        if (PRTE_SUCCESS != (ret = prte_dss_get_data_type(buffer, &remote_type))) {
            return ret;
        }
    }

    if (remote_type == DSS_TYPE_BOOL) {
        /* fast path it if the sizes are the same */
        /* Turn around and unpack the real type */
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, dest, num_vals, DSS_TYPE_BOOL))) {
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
int prte_dss_unpack_int(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type)
{
    int ret;
    prte_data_type_t remote_type;

    if (PRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        /* see what type was actually packed */
        if (PRTE_SUCCESS != (ret = prte_dss_peek_type(buffer, &remote_type))) {
            return ret;
        }
    } else {
        if (PRTE_SUCCESS != (ret = prte_dss_get_data_type(buffer, &remote_type))) {
            return ret;
        }
    }

    if (remote_type == DSS_TYPE_INT) {
        /* fast path it if the sizes are the same */
        /* Turn around and unpack the real type */
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, dest, num_vals, DSS_TYPE_INT))) {
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
int prte_dss_unpack_sizet(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type)
{
    int ret;
    prte_data_type_t remote_type;

    if (PRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        /* see what type was actually packed */
        if (PRTE_SUCCESS != (ret = prte_dss_peek_type(buffer, &remote_type))) {
            return ret;
        }
    } else {
        if (PRTE_SUCCESS != (ret = prte_dss_get_data_type(buffer, &remote_type))) {
            return ret;
        }
    }

    if (remote_type == DSS_TYPE_SIZE_T) {
        /* fast path it if the sizes are the same */
        /* Turn around and unpack the real type */
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, dest, num_vals, DSS_TYPE_SIZE_T))) {
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
int prte_dss_unpack_pid(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type)
{
    int ret;
    prte_data_type_t remote_type;

    if (PRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        /* see what type was actually packed */
        if (PRTE_SUCCESS != (ret = prte_dss_peek_type(buffer, &remote_type))) {
            return ret;
        }
    } else {
        if (PRTE_SUCCESS != (ret = prte_dss_get_data_type(buffer, &remote_type))) {
            return ret;
        }
    }

    if (remote_type == DSS_TYPE_PID_T) {
        /* fast path it if the sizes are the same */
        /* Turn around and unpack the real type */
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, dest, num_vals, DSS_TYPE_PID_T))) {
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
int prte_dss_unpack_null(prte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prte_data_type_t type)
{
    PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack_null * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prte_dss_too_small(buffer, *num_vals)) {
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    memcpy(dest, buffer->unpack_ptr, *num_vals);

    /* update buffer pointer */
    buffer->unpack_ptr += *num_vals;

    return PRTE_SUCCESS;
}

/*
 * BYTE, CHAR, INT8
 */
int prte_dss_unpack_byte(prte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prte_data_type_t type)
{
    PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack_byte * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prte_dss_too_small(buffer, *num_vals)) {
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    memcpy(dest, buffer->unpack_ptr, *num_vals);

    /* update buffer pointer */
    buffer->unpack_ptr += *num_vals;

    return PRTE_SUCCESS;
}

int prte_dss_unpack_int16(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type)
{
    int32_t i;
    uint16_t tmp, *desttmp = (uint16_t*) dest;

   PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack_int16 * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prte_dss_too_small(buffer, (*num_vals)*sizeof(tmp))) {
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        memcpy( &(tmp), buffer->unpack_ptr, sizeof(tmp) );
        tmp = ntohs(tmp);
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
        buffer->unpack_ptr += sizeof(tmp);
    }

    return PRTE_SUCCESS;
}

int prte_dss_unpack_int32(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type)
{
    int32_t i;
    uint32_t tmp, *desttmp = (uint32_t*) dest;

   PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack_int32 * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prte_dss_too_small(buffer, (*num_vals)*sizeof(tmp))) {
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        memcpy( &(tmp), buffer->unpack_ptr, sizeof(tmp) );
        tmp = ntohl(tmp);
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
        buffer->unpack_ptr += sizeof(tmp);
    }

    return PRTE_SUCCESS;
}

int prte_dss_unpack_int64(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type)
{
    int32_t i;
    uint64_t tmp, *desttmp = (uint64_t*) dest;

   PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack_int64 * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prte_dss_too_small(buffer, (*num_vals)*sizeof(tmp))) {
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        memcpy( &(tmp), buffer->unpack_ptr, sizeof(tmp) );
        tmp = prte_ntoh64(tmp);
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
        buffer->unpack_ptr += sizeof(tmp);
    }

    return PRTE_SUCCESS;
}

int prte_dss_unpack_string(prte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prte_data_type_t type)
{
    int ret;
    int32_t i, len, n=1;
    char **sdest = (char**) dest;

    for (i = 0; i < (*num_vals); ++i) {
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_int32(buffer, &len, &n, PRTE_INT32))) {
            return ret;
        }
        if (0 ==  len) {   /* zero-length string - unpack the NULL */
            sdest[i] = NULL;
        } else {
        sdest[i] = (char*)malloc(len);
            if (NULL == sdest[i]) {
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_byte(buffer, sdest[i], &len, PRTE_BYTE))) {
                return ret;
            }
        }
    }

    return PRTE_SUCCESS;
}

int prte_dss_unpack_float(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type)
{
    int32_t i, n;
    float *desttmp = (float*) dest, tmp;
    int ret;
    char *convert;

   PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack_float * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prte_dss_too_small(buffer, (*num_vals)*sizeof(float))) {
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        n=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_string(buffer, &convert, &n, PRTE_STRING))) {
            return ret;
        }
        if (NULL == convert) {
            return PRTE_ERR_UNPACK_FAILURE;
        }
        tmp = strtof(convert, NULL);
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
        free(convert);
        convert = NULL;
    }
    return PRTE_SUCCESS;
}

int prte_dss_unpack_double(prte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prte_data_type_t type)
{
    int32_t i, n;
    double *desttmp = (double*) dest, tmp;
    int ret;
    char *convert;

   PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack_double * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prte_dss_too_small(buffer, (*num_vals)*sizeof(double))) {
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        n=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_string(buffer, &convert, &n, PRTE_STRING))) {
            return ret;
        }
        if (NULL == convert) {
            return PRTE_ERR_UNPACK_FAILURE;
        }
        tmp = strtod(convert, NULL);
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
        free(convert);
        convert = NULL;
    }
    return PRTE_SUCCESS;
}

int prte_dss_unpack_timeval(prte_buffer_t *buffer, void *dest,
                            int32_t *num_vals, prte_data_type_t type)
{
    int32_t i, n;
    int64_t tmp[2];
    struct timeval *desttmp = (struct timeval *) dest, tt;
    int ret;

   PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack_timeval * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
    if (prte_dss_too_small(buffer, (*num_vals)*sizeof(struct timeval))) {
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        n=2;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_int64(buffer, tmp, &n, PRTE_INT64))) {
            return ret;
        }
        tt.tv_sec = tmp[0];
        tt.tv_usec = tmp[1];
        memcpy(&desttmp[i], &tt, sizeof(tt));
    }
    return PRTE_SUCCESS;
}

int prte_dss_unpack_time(prte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prte_data_type_t type)
{
    int32_t i, n;
    time_t *desttmp = (time_t *) dest, tmp;
    int ret;
    uint64_t ui64;

    /* time_t is a system-dependent size, so cast it
     * to uint64_t as a generic safe size
     */

   PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_unpack_time * %d\n", (int)*num_vals ) );
    /* check to see if there's enough data in buffer */
   if (prte_dss_too_small(buffer, (*num_vals)*(sizeof(uint64_t)))) {
        return PRTE_ERR_UNPACK_READ_PAST_END_OF_BUFFER;
    }

    /* unpack the data */
    for (i = 0; i < (*num_vals); ++i) {
        n=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_int64(buffer, &ui64, &n, PRTE_UINT64))) {
            return ret;
        }
        tmp = (time_t)ui64;
        memcpy(&desttmp[i], &tmp, sizeof(tmp));
    }
    return PRTE_SUCCESS;
}


/* UNPACK FUNCTIONS FOR GENERIC PRTE TYPES */

/*
 * PRTE_DATA_TYPE
 */
int prte_dss_unpack_data_type(prte_buffer_t *buffer, void *dest, int32_t *num_vals,
                             prte_data_type_t type)
{
     /* turn around and unpack the real type */
    return prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_DATA_TYPE_T);
}

/*
 * PRTE_BYTE_OBJECT
 */
int prte_dss_unpack_byte_object(prte_buffer_t *buffer, void *dest, int32_t *num,
                             prte_data_type_t type)
{
    int ret;
    int32_t i, n, m=1;
    prte_byte_object_t **dbyteptr;

    dbyteptr = (prte_byte_object_t**)dest;
    n = *num;
    for(i=0; i<n; i++) {
        /* allocate memory for the byte object itself */
        dbyteptr[i] = (prte_byte_object_t*)malloc(sizeof(prte_byte_object_t));
        if (NULL == dbyteptr[i]) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        /* unpack object size in bytes */
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_int32(buffer, &(dbyteptr[i]->size), &m, PRTE_INT32))) {
            return ret;
        }
        if (0 < dbyteptr[i]->size) {
            dbyteptr[i]->bytes = (uint8_t*)malloc(dbyteptr[i]->size);
            if (NULL == dbyteptr[i]->bytes) {
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_byte(buffer, (dbyteptr[i]->bytes),
                                            &(dbyteptr[i]->size), PRTE_BYTE))) {
                return ret;
            }
        } else {
            /* be sure to init the bytes pointer to NULL! */
            dbyteptr[i]->bytes = NULL;
        }
    }

    return PRTE_SUCCESS;
}

/*
 * PRTE_PSTAT
 */
int prte_dss_unpack_pstat(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type)
{
    prte_pstats_t **ptr;
    int32_t i, n, m;
    int ret;
    char *cptr;

    ptr = (prte_pstats_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRTE_NEW(prte_pstats_t);
        if (NULL == ptr[i]) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &cptr, &m, PRTE_STRING))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        memmove(ptr[i]->node, cptr, strlen(cptr));
        free(cptr);
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->rank, &m, PRTE_INT32))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->pid, &m, PRTE_PID))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &cptr, &m, PRTE_STRING))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        memmove(ptr[i]->cmd, cptr, strlen(cptr));
        free(cptr);
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->state[0], &m, PRTE_BYTE))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->time, &m, PRTE_TIMEVAL))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->priority, &m, PRTE_INT32))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->num_threads, &m, PRTE_INT16))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->pss, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->vsize, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->rss, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->peak_vsize, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->processor, &m, PRTE_INT16))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->sample_time, &m, PRTE_TIMEVAL))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
    }

    return PRTE_SUCCESS;
}

static int unpack_disk_stats(prte_buffer_t *buffer, prte_node_stats_t *ns)
{
    int32_t i, m, n;
    int ret;
    prte_diskstats_t *dk;
    uint64_t i64;

    /* unpack the number of disk stat objects */
    m=1;
    if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &n, &m, PRTE_INT32))) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }
    /* unpack them */
    for (i=0; i < n; i++) {
        dk = PRTE_NEW(prte_diskstats_t);
        assert(dk);
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &dk->disk, &m, PRTE_STRING))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        dk->num_reads_completed = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        dk->num_reads_merged = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        dk->num_sectors_read = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        dk->milliseconds_reading = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        dk->num_writes_completed = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        dk->num_writes_merged = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        dk->num_sectors_written = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        dk->milliseconds_writing = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        dk->num_ios_in_progress = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        dk->milliseconds_io = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(dk);
            return ret;
        }
        dk->weighted_milliseconds_io = i64;
        prte_list_append(&ns->diskstats, &dk->super);
    }
    return PRTE_SUCCESS;
}

static int unpack_net_stats(prte_buffer_t *buffer, prte_node_stats_t *ns)
{
    int32_t i, m, n;
    int ret;
    prte_netstats_t *net;
    uint64_t i64;

    /* unpack the number of net stat objects */
    m=1;
    if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &n, &m, PRTE_INT32))) {
        PRTE_ERROR_LOG(ret);
        return ret;
    }
    /* unpack them */
    for (i=0; i < n; i++) {
        net = PRTE_NEW(prte_netstats_t);
        assert(net);
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &net->net_interface, &m, PRTE_STRING))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(net);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(net);
            return ret;
        }
        net->num_bytes_recvd = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(net);
            return ret;
        }
        net->num_packets_recvd = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(net);
            return ret;
        }
        net->num_recv_errs = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(net);
            return ret;
        }
        net->num_bytes_sent = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(net);
            return ret;
        }
        net->num_packets_sent = i64;
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &i64, &m, PRTE_UINT64))) {
            PRTE_ERROR_LOG(ret);
            PRTE_RELEASE(net);
            return ret;
        }
        net->num_send_errs = i64;
        prte_list_append(&ns->netstats, &net->super);
    }
    return PRTE_SUCCESS;
}

/*
 * PRTE_NODE_STAT
 */
int prte_dss_unpack_node_stat(prte_buffer_t *buffer, void *dest,
                              int32_t *num_vals, prte_data_type_t type)
{
    prte_node_stats_t **ptr;
    int32_t i, n, m;
    int ret;

    ptr = (prte_node_stats_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRTE_NEW(prte_node_stats_t);
        if (NULL == ptr[i]) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->la, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->la5, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->la15, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->total_mem, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->free_mem, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->buffers, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->cached, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->swap_cached, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->swap_total, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->swap_free, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_float(buffer, &ptr[i]->mapped, &m, PRTE_FLOAT))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->sample_time, &m, PRTE_TIMEVAL))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        /* unpack the disk stat objects */
        if (PRTE_SUCCESS != (ret = unpack_disk_stats(buffer, ptr[i]))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        /* unpack the net stat objects */
        if (PRTE_SUCCESS != (ret = unpack_net_stats(buffer, ptr[i]))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        PRTE_RELEASE(ptr[i]);
    }

    return PRTE_SUCCESS;
}

/*
 * PRTE_VALUE
 */
int prte_dss_unpack_value(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type)
{
    prte_value_t **ptr;
    int32_t i, n, m;
    int ret;

    ptr = (prte_value_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRTE_NEW(prte_value_t);
        if (NULL == ptr[i]) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        /* unpack the key and type */
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_string(buffer, &ptr[i]->key, &m, PRTE_STRING))) {
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_data_type(buffer, &ptr[i]->type, &m, PRTE_DATA_TYPE))) {
            return ret;
        }
        /* now unpack the right field */
        m=1;
        switch (ptr[i]->type) {
        case PRTE_BOOL:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.flag, &m, PRTE_BOOL))) {
                return ret;
            }
            break;
        case PRTE_BYTE:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.byte, &m, PRTE_BYTE))) {
                return ret;
            }
            break;
        case PRTE_STRING:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.string, &m, PRTE_STRING))) {
                return ret;
            }
            break;
        case PRTE_SIZE:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.size, &m, PRTE_SIZE))) {
                return ret;
            }
            break;
        case PRTE_PID:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.pid, &m, PRTE_PID))) {
                return ret;
            }
            break;
        case PRTE_INT:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.integer, &m, PRTE_INT))) {
                return ret;
            }
            break;
        case PRTE_INT8:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.int8, &m, PRTE_INT8))) {
                return ret;
            }
            break;
        case PRTE_INT16:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.int16, &m, PRTE_INT16))) {
                return ret;
            }
            break;
        case PRTE_INT32:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.int32, &m, PRTE_INT32))) {
                return ret;
            }
            break;
        case PRTE_INT64:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.int64, &m, PRTE_INT64))) {
                return ret;
            }
            break;
        case PRTE_UINT:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.uint, &m, PRTE_UINT))) {
                return ret;
            }
            break;
        case PRTE_UINT8:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.uint8, &m, PRTE_UINT8))) {
                return ret;
            }
            break;
        case PRTE_UINT16:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.uint16, &m, PRTE_UINT16))) {
                return ret;
            }
            break;
        case PRTE_UINT32:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.uint32, &m, PRTE_UINT32))) {
                return ret;
            }
            break;
        case PRTE_UINT64:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.uint64, &m, PRTE_UINT64))) {
                return ret;
            }
            break;
        case PRTE_BYTE_OBJECT:
            /* cannot use byte object unpack as it allocates memory, so unpack object size in bytes */
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_int32(buffer, &(ptr[i]->data.bo.size), &m, PRTE_INT32))) {
                return ret;
            }
            if (0 < ptr[i]->data.bo.size) {
                ptr[i]->data.bo.bytes = (uint8_t*)malloc(ptr[i]->data.bo.size);
                if (NULL == ptr[i]->data.bo.bytes) {
                    return PRTE_ERR_OUT_OF_RESOURCE;
                }
                if (PRTE_SUCCESS != (ret = prte_dss_unpack_byte(buffer, ptr[i]->data.bo.bytes,
                                                                &(ptr[i]->data.bo.size), PRTE_BYTE))) {
                    return ret;
                }
            } else {
                ptr[i]->data.bo.bytes = NULL;
            }
            break;
        case PRTE_FLOAT:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.fval, &m, PRTE_FLOAT))) {
                return ret;
            }
            break;
        case PRTE_DOUBLE:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.dval, &m, PRTE_DOUBLE))) {
                return ret;
            }
            break;
        case PRTE_TIMEVAL:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.tv, &m, PRTE_TIMEVAL))) {
                return ret;
            }
            break;
        case PRTE_TIME:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.time, &m, PRTE_TIME))) {
                return ret;
            }
            break;
        case PRTE_PTR:
            /* just ignore these values */
            break;
        case PRTE_NAME:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.name, &m, PRTE_NAME))) {
                return ret;
            }
            break;
        case PRTE_STATUS:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.status, &m, PRTE_INT))) {
                return ret;
            }
            break;
        case PRTE_ENVAR:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.envar, &m, PRTE_ENVAR))) {
                return ret;
            }
            break;
        default:
            prte_output(0, "UNPACK-PRTE-VALUE: UNSUPPORTED TYPE %d FOR KEY %s", (int)ptr[i]->type, ptr[i]->key);
            return PRTE_ERROR;
        }
    }

    return PRTE_SUCCESS;
}

/*
 * PRTE_BUFFER
 */
int prte_dss_unpack_buffer_contents(prte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prte_data_type_t type)
{
    prte_buffer_t **ptr;
    int32_t i, n, m;
    int ret;
    size_t nbytes;

    ptr = (prte_buffer_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRTE_NEW(prte_buffer_t);
        if (NULL == ptr[i]) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        /* unpack the number of bytes */
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_sizet(buffer, &nbytes, &m, PRTE_SIZE))) {
            return ret;
        }
        m = nbytes;
        /* setup the buffer's data region */
        if (0 < nbytes) {
            ptr[i]->base_ptr = (char*)malloc(nbytes);
            /* unpack the bytes */
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_byte(buffer, ptr[i]->base_ptr, &m, PRTE_BYTE))) {
                return ret;
            }
        }
        ptr[i]->pack_ptr = ptr[i]->base_ptr + m;
        ptr[i]->unpack_ptr = ptr[i]->base_ptr;
        ptr[i]->bytes_allocated = nbytes;
        ptr[i]->bytes_used = m;
    }
    return PRTE_SUCCESS;
}

/*
 * NAME
 */
int prte_dss_unpack_name(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type)
{
    int rc;
    int32_t i, num;
    prte_process_name_t* proc;
    prte_jobid_t *jobid;
    prte_vpid_t *vpid;

    num = *num_vals;

    /* allocate space for all the jobids in a contiguous array */
    jobid = (prte_jobid_t*)malloc(num * sizeof(prte_jobid_t));
    if (NULL == jobid) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        *num_vals = 0;
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    /* now unpack them in one shot */
    if (PRTE_SUCCESS != (rc =
                         prte_dss_unpack_jobid(buffer, jobid, num_vals, PRTE_JOBID))) {
        PRTE_ERROR_LOG(rc);
        *num_vals = 0;
        free(jobid);
        return rc;
    }

    /* collect all the vpids in a contiguous array */
    vpid = (prte_vpid_t*)malloc(num * sizeof(prte_vpid_t));
    if (NULL == vpid) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        *num_vals = 0;
        free(jobid);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    /* now unpack them in one shot */
    if (PRTE_SUCCESS != (rc =
                         prte_dss_unpack_vpid(buffer, vpid, num_vals, PRTE_VPID))) {
        PRTE_ERROR_LOG(rc);
        *num_vals = 0;
        free(vpid);
        free(jobid);
        return rc;
    }

    /* build the names from the jobid/vpid arrays */
    proc = (prte_process_name_t*)dest;
    for (i=0; i < num; i++) {
        proc->jobid = jobid[i];
        proc->vpid = vpid[i];
        proc++;
    }

    /* cleanup */
    free(vpid);
    free(jobid);

    return PRTE_SUCCESS;
}

/*
 * JOBID
 */
int prte_dss_unpack_jobid(prte_buffer_t *buffer, void *dest,
                         int32_t *num_vals, prte_data_type_t type)
{
    int ret;

    /* Turn around and unpack the real type */
    if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_JOBID_T))) {
        PRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * VPID
 */
int prte_dss_unpack_vpid(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type)
{
    int ret;

    /* Turn around and unpack the real type */
    if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_VPID_T))) {
        PRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * STATUS
 */
int prte_dss_unpack_status(prte_buffer_t *buffer, void *dest,
                           int32_t *num_vals, prte_data_type_t type)
{
    int ret;

    /* Turn around and unpack the real type */
    ret = prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_INT);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
    }

    return ret;
}


int prte_dss_unpack_envar(prte_buffer_t *buffer, void *dest,
                          int32_t *num_vals, prte_data_type_t type)
{
    prte_envar_t *ptr;
    int32_t i, n, m;
    int ret;

    ptr = (prte_envar_t *) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_string(buffer, &ptr[i].envar, &m, PRTE_STRING))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_string(buffer, &ptr[i].value, &m, PRTE_STRING))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_byte(buffer, &ptr[i].separator, &m, PRTE_BYTE))) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
    }

    return PRTE_SUCCESS;
}
