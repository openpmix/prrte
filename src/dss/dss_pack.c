/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2007 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "types.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/dss/dss_internal.h"

int prrte_dss_pack(prrte_buffer_t *buffer,
                  const void *src, int32_t num_vals,
                  prrte_data_type_t type)
{
    int rc;

    /* check for error */
    if (NULL == buffer) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* Pack the number of values */
    if (PRRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        if (PRRTE_SUCCESS != (rc = prrte_dss_store_data_type(buffer, PRRTE_INT32))) {
            return rc;
        }
    }
    if (PRRTE_SUCCESS != (rc = prrte_dss_pack_int32(buffer, &num_vals, 1, PRRTE_INT32))) {
        return rc;
    }

    /* Pack the value(s) */
    return prrte_dss_pack_buffer(buffer, src, num_vals, type);
}

int prrte_dss_pack_buffer(prrte_buffer_t *buffer,
                         const void *src, int32_t num_vals,
                         prrte_data_type_t type)
{
    int rc;
    prrte_dss_type_info_t *info;

    PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_pack_buffer( %p, %p, %lu, %d )\n",
                   (void*)buffer, src, (long unsigned int)num_vals, (int)type ) );

    /* Pack the declared data type */
    if (PRRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        if (PRRTE_SUCCESS != (rc = prrte_dss_store_data_type(buffer, type))) {
            return rc;
        }
    }

    /* Lookup the pack function for this type and call it */

    if (NULL == (info = (prrte_dss_type_info_t*)prrte_pointer_array_get_item(&prrte_dss_types, type))) {
        return PRRTE_ERR_PACK_FAILURE;
    }

    return info->odti_pack_fn(buffer, src, num_vals, type);
}


/* PACK FUNCTIONS FOR GENERIC SYSTEM TYPES */

/*
 * BOOL
 */
int prrte_dss_pack_bool(prrte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prrte_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
       unpack them.  If we aren't fully described, then add the
       description for this type... */
    if (PRRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        if (PRRTE_SUCCESS != (ret = prrte_dss_store_data_type(buffer, DSS_TYPE_BOOL))) {
            return ret;
        }
    }

    /* Turn around and pack the real type */
    return prrte_dss_pack_buffer(buffer, src, num_vals, DSS_TYPE_BOOL);
}

/*
 * INT
 */
int prrte_dss_pack_int(prrte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prrte_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
       unpack them.  If we aren't fully described, then add the
       description for this type... */
    if (PRRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        if (PRRTE_SUCCESS != (ret = prrte_dss_store_data_type(buffer, DSS_TYPE_INT))) {
            return ret;
        }
    }

    /* Turn around and pack the real type */
    return prrte_dss_pack_buffer(buffer, src, num_vals, DSS_TYPE_INT);
}

/*
 * SIZE_T
 */
int prrte_dss_pack_sizet(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
       unpack them.  If we aren't fully described, then add the
       description for this type... */
    if (PRRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        if (PRRTE_SUCCESS != (ret = prrte_dss_store_data_type(buffer, DSS_TYPE_SIZE_T))) {
            return ret;
        }
    }

    return prrte_dss_pack_buffer(buffer, src, num_vals, DSS_TYPE_SIZE_T);
}

/*
 * PID_T
 */
int prrte_dss_pack_pid(prrte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prrte_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
       unpack them.  If we aren't fully described, then add the
       description for this type... */
    if (PRRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        if (PRRTE_SUCCESS != (ret = prrte_dss_store_data_type(buffer, DSS_TYPE_PID_T))) {
            return ret;
        }
    }

    /* Turn around and pack the real type */
    return prrte_dss_pack_buffer(buffer, src, num_vals, DSS_TYPE_PID_T);
}


/* PACK FUNCTIONS FOR NON-GENERIC SYSTEM TYPES */

/*
 * NULL
 */
int prrte_dss_pack_null(prrte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prrte_data_type_t type)
{
    char null=0x00;
    char *dst;

    PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_pack_null * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = prrte_dss_buffer_extend(buffer, num_vals))) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    /* store the nulls */
    memset(dst, (int)null, num_vals);

    /* update buffer pointers */
    buffer->pack_ptr += num_vals;
    buffer->bytes_used += num_vals;

    return PRRTE_SUCCESS;
}

/*
 * BYTE, CHAR, INT8
 */
int prrte_dss_pack_byte(prrte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prrte_data_type_t type)
{
    char *dst;

    PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_pack_byte * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = prrte_dss_buffer_extend(buffer, num_vals))) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    /* store the data */
    memcpy(dst, src, num_vals);

    /* update buffer pointers */
    buffer->pack_ptr += num_vals;
    buffer->bytes_used += num_vals;

    return PRRTE_SUCCESS;
}

/*
 * INT16
 */
int prrte_dss_pack_int16(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type)
{
    int32_t i;
    uint16_t tmp, *srctmp = (uint16_t*) src;
    char *dst;

    PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_pack_int16 * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = prrte_dss_buffer_extend(buffer, num_vals*sizeof(tmp)))) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0; i < num_vals; ++i) {
        tmp = htons(srctmp[i]);
        memcpy(dst, &tmp, sizeof(tmp));
        dst += sizeof(tmp);
    }
    buffer->pack_ptr += num_vals * sizeof(tmp);
    buffer->bytes_used += num_vals * sizeof(tmp);

    return PRRTE_SUCCESS;
}

/*
 * INT32
 */
int prrte_dss_pack_int32(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type)
{
    int32_t i;
    uint32_t tmp, *srctmp = (uint32_t*) src;
    char *dst;

    PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_pack_int32 * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = prrte_dss_buffer_extend(buffer, num_vals*sizeof(tmp)))) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0; i < num_vals; ++i) {
        tmp = htonl(srctmp[i]);
        memcpy(dst, &tmp, sizeof(tmp));
        dst += sizeof(tmp);
    }
    buffer->pack_ptr += num_vals * sizeof(tmp);
    buffer->bytes_used += num_vals * sizeof(tmp);

    return PRRTE_SUCCESS;
}

/*
 * INT64
 */
int prrte_dss_pack_int64(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type)
{
    int32_t i;
    uint64_t tmp, *srctmp = (uint64_t*) src;
    char *dst;
    size_t bytes_packed = num_vals * sizeof(tmp);

    PRRTE_OUTPUT( ( prrte_dss_verbose, "prrte_dss_pack_int64 * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = prrte_dss_buffer_extend(buffer, bytes_packed))) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0; i < num_vals; ++i) {
        tmp = prrte_hton64(srctmp[i]);
        memcpy(dst, &tmp, sizeof(tmp));
        dst += sizeof(tmp);
    }
    buffer->pack_ptr += bytes_packed;
    buffer->bytes_used += bytes_packed;

    return PRRTE_SUCCESS;
}

/*
 * STRING
 */
int prrte_dss_pack_string(prrte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prrte_data_type_t type)
{
    int ret = PRRTE_SUCCESS;
    int32_t i, len;
    char **ssrc = (char**) src;

    for (i = 0; i < num_vals; ++i) {
        if (NULL == ssrc[i]) {  /* got zero-length string/NULL pointer - store NULL */
            len = 0;
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_int32(buffer, &len, 1, PRRTE_INT32))) {
                return ret;
            }
        } else {
            len = (int32_t)strlen(ssrc[i]) + 1;
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_int32(buffer, &len, 1, PRRTE_INT32))) {
                return ret;
            }
            if (PRRTE_SUCCESS != (ret =
                prrte_dss_pack_byte(buffer, ssrc[i], len, PRRTE_BYTE))) {
                return ret;
            }
        }
    }

    return PRRTE_SUCCESS;
}

/* FLOAT */
int prrte_dss_pack_float(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type)
{
    int ret = PRRTE_SUCCESS;
    int32_t i;
    float *ssrc = (float*)src;
    char *convert;

    for (i = 0; i < num_vals; ++i) {
        prrte_asprintf(&convert, "%f", ssrc[i]);
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_string(buffer, &convert, 1, PRRTE_STRING))) {
            free(convert);
            return ret;
        }
        free(convert);
    }

    return PRRTE_SUCCESS;
}

/* DOUBLE */
int prrte_dss_pack_double(prrte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prrte_data_type_t type)
{
    int ret = PRRTE_SUCCESS;
    int32_t i;
    double *ssrc = (double*)src;
    char *convert;

    for (i = 0; i < num_vals; ++i) {
        prrte_asprintf(&convert, "%f", ssrc[i]);
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_string(buffer, &convert, 1, PRRTE_STRING))) {
            free(convert);
            return ret;
        }
        free(convert);
    }

    return PRRTE_SUCCESS;
}

/* TIMEVAL */
int prrte_dss_pack_timeval(prrte_buffer_t *buffer, const void *src,
                          int32_t num_vals, prrte_data_type_t type)
{
    int64_t tmp[2];
    int ret = PRRTE_SUCCESS;
    int32_t i;
    struct timeval *ssrc = (struct timeval *)src;

    for (i = 0; i < num_vals; ++i) {
        tmp[0] = (int64_t)ssrc[i].tv_sec;
        tmp[1] = (int64_t)ssrc[i].tv_usec;
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_int64(buffer, tmp, 2, PRRTE_INT64))) {
            return ret;
        }
    }

    return PRRTE_SUCCESS;
}

/* TIME */
int prrte_dss_pack_time(prrte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prrte_data_type_t type)
{
    int ret = PRRTE_SUCCESS;
    int32_t i;
    time_t *ssrc = (time_t *)src;
    uint64_t ui64;

    /* time_t is a system-dependent size, so cast it
     * to uint64_t as a generic safe size
     */
    for (i = 0; i < num_vals; ++i) {
        ui64 = (uint64_t)ssrc[i];
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_int64(buffer, &ui64, 1, PRRTE_UINT64))) {
            return ret;
        }
    }

    return PRRTE_SUCCESS;
}


/* PACK FUNCTIONS FOR GENERIC PRRTE TYPES */

/*
 * PRRTE_DATA_TYPE
 */
int prrte_dss_pack_data_type(prrte_buffer_t *buffer, const void *src, int32_t num_vals,
                            prrte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_DATA_TYPE_T))) {
    }

    return ret;
}

/*
 * PRRTE_BYTE_OBJECT
 */
int prrte_dss_pack_byte_object(prrte_buffer_t *buffer, const void *src, int32_t num,
                             prrte_data_type_t type)
{
    prrte_byte_object_t **sbyteptr;
    int32_t i, n;
    int ret;

    sbyteptr = (prrte_byte_object_t **) src;

    for (i = 0; i < num; ++i) {
        n = sbyteptr[i]->size;
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_int32(buffer, &n, 1, PRRTE_INT32))) {
            return ret;
        }
        if (0 < n) {
            if (PRRTE_SUCCESS != (ret =
                prrte_dss_pack_byte(buffer, sbyteptr[i]->bytes, n, PRRTE_BYTE))) {
                return ret;
            }
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * PRRTE_PSTAT
 */
int prrte_dss_pack_pstat(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type)
{
    prrte_pstats_t **ptr;
    int32_t i;
    int ret;
    char *cptr;

    ptr = (prrte_pstats_t **) src;

    for (i = 0; i < num_vals; ++i) {
        cptr = ptr[i]->node;
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &cptr, 1, PRRTE_STRING))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->rank, 1, PRRTE_INT32))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->pid, 1, PRRTE_PID))) {
            return ret;
        }
        cptr = ptr[i]->cmd;
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &cptr, 1, PRRTE_STRING))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->state[0], 1, PRRTE_BYTE))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->time, 1, PRRTE_TIMEVAL))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->priority, 1, PRRTE_INT32))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->num_threads, 1, PRRTE_INT16))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->pss, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->vsize, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->rss, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->peak_vsize, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->processor, 1, PRRTE_INT16))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->sample_time, 1, PRRTE_TIMEVAL))) {
            return ret;
        }
    }

    return PRRTE_SUCCESS;
}

static int pack_disk_stats(prrte_buffer_t *buffer, prrte_diskstats_t *dk)
{
    uint64_t i64;
    int ret;

    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &dk->disk, 1, PRRTE_STRING))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_reads_completed;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_reads_merged;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_sectors_read;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->milliseconds_reading;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_writes_completed;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_writes_merged;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_sectors_written;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->milliseconds_writing;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_ios_in_progress;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->milliseconds_io;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->weighted_milliseconds_io;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    return PRRTE_SUCCESS;
}

static int pack_net_stats(prrte_buffer_t *buffer, prrte_netstats_t *ns)
{
    uint64_t i64;
    int ret;

    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ns->net_interface, 1, PRRTE_STRING))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_bytes_recvd;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_packets_recvd;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_recv_errs;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_bytes_sent;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_packets_sent;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_send_errs;
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &i64, 1, PRRTE_UINT64))) {
        return ret;
    }
    return PRRTE_SUCCESS;
}

/*
 * PRRTE_NODE_STAT
 */
int prrte_dss_pack_node_stat(prrte_buffer_t *buffer, const void *src,
                            int32_t num_vals, prrte_data_type_t type)
{
    prrte_node_stats_t **ptr;
    int32_t i, j;
    int ret;
    prrte_diskstats_t *ds;
    prrte_netstats_t *ns;

    ptr = (prrte_node_stats_t **) src;

    for (i = 0; i < num_vals; ++i) {
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->la, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->la5, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->la15, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->total_mem, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->free_mem, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->buffers, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->cached, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->swap_cached, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->swap_total, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->swap_free, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_float(buffer, &ptr[i]->mapped, 1, PRRTE_FLOAT))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->sample_time, 1, PRRTE_TIMEVAL))) {
            return ret;
        }
        /* pack the number of disk stat objects on the list */
        j = prrte_list_get_size(&ptr[i]->diskstats);
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &j, 1, PRRTE_INT32))) {
            return ret;
        }
        if (0 < j) {
            /* pack them */
            PRRTE_LIST_FOREACH(ds, &ptr[i]->diskstats, prrte_diskstats_t) {
                if (PRRTE_SUCCESS != (ret = pack_disk_stats(buffer, ds))) {
                    return ret;
                }
            }
        }
        /* pack the number of net stat objects on the list */
        j = prrte_list_get_size(&ptr[i]->netstats);
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &j, 1, PRRTE_INT32))) {
            return ret;
        }
        if (0 < j) {
            /* pack them */
            PRRTE_LIST_FOREACH(ns, &ptr[i]->netstats, prrte_netstats_t) {
                if (PRRTE_SUCCESS != (ret = pack_net_stats(buffer, ns))) {
                    return ret;
                }
            }
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * PRRTE_VALUE
 */
int prrte_dss_pack_value(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type)
{
    prrte_value_t **ptr;
    int32_t i, n;
    int ret;

    ptr = (prrte_value_t **) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack the key and type */
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_string(buffer, &ptr[i]->key, 1, PRRTE_STRING))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_data_type(buffer, &ptr[i]->type, 1, PRRTE_DATA_TYPE))) {
            return ret;
        }
        /* now pack the right field */
        switch (ptr[i]->type) {
        case PRRTE_BOOL:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.flag, 1, PRRTE_BOOL))) {
                return ret;
            }
            break;
        case PRRTE_BYTE:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.byte, 1, PRRTE_BYTE))) {
                return ret;
            }
            break;
        case PRRTE_STRING:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.string, 1, PRRTE_STRING))) {
                return ret;
            }
            break;
        case PRRTE_SIZE:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.size, 1, PRRTE_SIZE))) {
                return ret;
            }
            break;
        case PRRTE_PID:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.pid, 1, PRRTE_PID))) {
                return ret;
            }
            break;
        case PRRTE_INT:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.integer, 1, PRRTE_INT))) {
                return ret;
            }
            break;
        case PRRTE_INT8:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.int8, 1, PRRTE_INT8))) {
                return ret;
            }
            break;
        case PRRTE_INT16:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.int16, 1, PRRTE_INT16))) {
                return ret;
            }
            break;
        case PRRTE_INT32:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.int32, 1, PRRTE_INT32))) {
                return ret;
            }
            break;
        case PRRTE_INT64:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.int64, 1, PRRTE_INT64))) {
                return ret;
            }
            break;
        case PRRTE_UINT:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.uint, 1, PRRTE_UINT))) {
                return ret;
            }
            break;
        case PRRTE_UINT8:
        case PRRTE_PERSIST:
        case PRRTE_SCOPE:
        case PRRTE_DATA_RANGE:
        case PRRTE_PROC_STATE:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.uint8, 1, PRRTE_UINT8))) {
                return ret;
            }
            break;
        case PRRTE_UINT16:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.uint16, 1, PRRTE_UINT16))) {
                return ret;
            }
            break;
        case PRRTE_UINT32:
        case PRRTE_INFO_DIRECTIVES:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.uint32, 1, PRRTE_UINT32))) {
                return ret;
            }
            break;
        case PRRTE_UINT64:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.uint64, 1, PRRTE_UINT64))) {
                return ret;
            }
            break;
        case PRRTE_BYTE_OBJECT:
            /* have to pack by hand so we can match unpack without allocation */
            n = ptr[i]->data.bo.size;
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_int32(buffer, &n, 1, PRRTE_INT32))) {
                return ret;
            }
            if (0 < n) {
                if (PRRTE_SUCCESS != (ret = prrte_dss_pack_byte(buffer, ptr[i]->data.bo.bytes, n, PRRTE_BYTE))) {
                    return ret;
                }
            }
            break;
        case PRRTE_FLOAT:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.fval, 1, PRRTE_FLOAT))) {
                return ret;
            }
            break;
        case PRRTE_DOUBLE:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.dval, 1, PRRTE_DOUBLE))) {
                return ret;
            }
            break;
        case PRRTE_TIMEVAL:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.tv, 1, PRRTE_TIMEVAL))) {
                return ret;
            }
            break;
        case PRRTE_TIME:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.time, 1, PRRTE_TIME))) {
                return ret;
            }
            break;
        case PRRTE_PTR:
            /* just ignore these values */
            break;
        case PRRTE_NAME:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.name, 1, PRRTE_NAME))) {
                return ret;
            }
            break;
        case PRRTE_STATUS:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.status, 1, PRRTE_INT))) {
                return ret;
            }
            break;
        case PRRTE_ENVAR:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.envar, 1, PRRTE_ENVAR))) {
                return ret;
            }
            break;
        default:
            prrte_output(0, "PACK-PRRTE-VALUE: UNSUPPORTED TYPE %d FOR KEY %s", (int)ptr[i]->type, ptr[i]->key);
            return PRRTE_ERROR;
        }
    }

    return PRRTE_SUCCESS;
}


/*
 * BUFFER CONTENTS
 */
int prrte_dss_pack_buffer_contents(prrte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prrte_data_type_t type)
{
    prrte_buffer_t **ptr;
    int32_t i;
    int ret;

    ptr = (prrte_buffer_t **) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack the number of bytes */
        PRRTE_OUTPUT((prrte_dss_verbose, "prrte_dss_pack_buffer_contents: bytes_used %u\n", (unsigned)ptr[i]->bytes_used));
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_sizet(buffer, &ptr[i]->bytes_used, 1, PRRTE_SIZE))) {
            return ret;
        }
        /* pack the bytes */
        if (0 < ptr[i]->bytes_used) {
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_byte(buffer, ptr[i]->base_ptr, ptr[i]->bytes_used, PRRTE_BYTE))) {
                return ret;
            }
        } else {
            ptr[i]->base_ptr = NULL;
        }
    }
    return PRRTE_SUCCESS;
}

/*
 * NAME
 */
int prrte_dss_pack_name(prrte_buffer_t *buffer, const void *src,
                           int32_t num_vals, prrte_data_type_t type)
{
    int rc;
    int32_t i;
    prrte_process_name_t* proc;
    prrte_jobid_t *jobid;
    prrte_vpid_t *vpid;

    /* collect all the jobids in a contiguous array */
    jobid = (prrte_jobid_t*)malloc(num_vals * sizeof(prrte_jobid_t));
    if (NULL == jobid) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    proc = (prrte_process_name_t*)src;
    for (i=0; i < num_vals; i++) {
        jobid[i] = proc->jobid;
        proc++;
    }
    /* now pack them in one shot */
    if (PRRTE_SUCCESS != (rc =
                         prrte_dss_pack_jobid(buffer, jobid, num_vals, PRRTE_JOBID))) {
        PRRTE_ERROR_LOG(rc);
        free(jobid);
        return rc;
    }
    free(jobid);

    /* collect all the vpids in a contiguous array */
    vpid = (prrte_vpid_t*)malloc(num_vals * sizeof(prrte_vpid_t));
    if (NULL == vpid) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    proc = (prrte_process_name_t*)src;
    for (i=0; i < num_vals; i++) {
        vpid[i] = proc->vpid;
        proc++;
    }
    /* now pack them in one shot */
    if (PRRTE_SUCCESS != (rc =
                         prrte_dss_pack_vpid(buffer, vpid, num_vals, PRRTE_VPID))) {
        PRRTE_ERROR_LOG(rc);
        free(vpid);
        return rc;
    }
    free(vpid);

    return PRRTE_SUCCESS;
}

/*
 * JOBID
 */
int prrte_dss_pack_jobid(prrte_buffer_t *buffer, const void *src,
                            int32_t num_vals, prrte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    if (PRRTE_SUCCESS != (
                         ret = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_JOBID_T))) {
        PRRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * VPID
 */
int prrte_dss_pack_vpid(prrte_buffer_t *buffer, const void *src,
                           int32_t num_vals, prrte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    if (PRRTE_SUCCESS != (
                         ret = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_VPID_T))) {
        PRRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * STATUS
 */
int prrte_dss_pack_status(prrte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prrte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    ret = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_INT);
    if (PRRTE_SUCCESS != ret) {
        PRRTE_ERROR_LOG(ret);
    }

    return ret;
}

int prrte_dss_pack_envar(prrte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prrte_data_type_t type)
{
    int ret;
    int32_t n;
    prrte_envar_t *ptr = (prrte_envar_t*)src;

    for (n=0; n < num_vals; n++) {
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_string(buffer, &ptr[n].envar, 1, PRRTE_STRING))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_string(buffer, &ptr[n].value, 1, PRRTE_STRING))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_byte(buffer, &ptr[n].separator, 1, PRRTE_BYTE))) {
            return ret;
        }
    }
    return PRRTE_SUCCESS;
}
