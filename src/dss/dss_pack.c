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
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
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

#include "prte_config.h"

#include "types.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/dss/dss_internal.h"

int prte_dss_pack(prte_buffer_t *buffer,
                  const void *src, int32_t num_vals,
                  prte_data_type_t type)
{
    int rc;

    /* check for error */
    if (NULL == buffer) {
        return PRTE_ERR_BAD_PARAM;
    }

    /* Pack the number of values */
    if (PRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        if (PRTE_SUCCESS != (rc = prte_dss_store_data_type(buffer, PRTE_INT32))) {
            return rc;
        }
    }
    if (PRTE_SUCCESS != (rc = prte_dss_pack_int32(buffer, &num_vals, 1, PRTE_INT32))) {
        return rc;
    }

    /* Pack the value(s) */
    return prte_dss_pack_buffer(buffer, src, num_vals, type);
}

int prte_dss_pack_buffer(prte_buffer_t *buffer,
                         const void *src, int32_t num_vals,
                         prte_data_type_t type)
{
    int rc;
    prte_dss_type_info_t *info;

    PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_pack_buffer( %p, %p, %lu, %d )\n",
                   (void*)buffer, src, (long unsigned int)num_vals, (int)type ) );

    /* Pack the declared data type */
    if (PRTE_DSS_BUFFER_FULLY_DESC == buffer->type) {
        if (PRTE_SUCCESS != (rc = prte_dss_store_data_type(buffer, type))) {
            return rc;
        }
    }

    /* Lookup the pack function for this type and call it */

    if (NULL == (info = (prte_dss_type_info_t*)prte_pointer_array_get_item(&prte_dss_types, type))) {
        return PRTE_ERR_PACK_FAILURE;
    }

    return info->odti_pack_fn(buffer, src, num_vals, type);
}


/* PACK FUNCTIONS FOR GENERIC SYSTEM TYPES */

/*
 * BOOL
 */
int prte_dss_pack_bool(prte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prte_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
       unpack them.  If we aren't fully described, then add the
       description for this type... */
    if (PRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        if (PRTE_SUCCESS != (ret = prte_dss_store_data_type(buffer, DSS_TYPE_BOOL))) {
            return ret;
        }
    }

    /* Turn around and pack the real type */
    return prte_dss_pack_buffer(buffer, src, num_vals, DSS_TYPE_BOOL);
}

/*
 * INT
 */
int prte_dss_pack_int(prte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prte_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
       unpack them.  If we aren't fully described, then add the
       description for this type... */
    if (PRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        if (PRTE_SUCCESS != (ret = prte_dss_store_data_type(buffer, DSS_TYPE_INT))) {
            return ret;
        }
    }

    /* Turn around and pack the real type */
    return prte_dss_pack_buffer(buffer, src, num_vals, DSS_TYPE_INT);
}

/*
 * SIZE_T
 */
int prte_dss_pack_sizet(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
       unpack them.  If we aren't fully described, then add the
       description for this type... */
    if (PRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        if (PRTE_SUCCESS != (ret = prte_dss_store_data_type(buffer, DSS_TYPE_SIZE_T))) {
            return ret;
        }
    }

    return prte_dss_pack_buffer(buffer, src, num_vals, DSS_TYPE_SIZE_T);
}

/*
 * PID_T
 */
int prte_dss_pack_pid(prte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prte_data_type_t type)
{
    int ret;

    /* System types need to always be described so we can properly
       unpack them.  If we aren't fully described, then add the
       description for this type... */
    if (PRTE_DSS_BUFFER_FULLY_DESC != buffer->type) {
        if (PRTE_SUCCESS != (ret = prte_dss_store_data_type(buffer, DSS_TYPE_PID_T))) {
            return ret;
        }
    }

    /* Turn around and pack the real type */
    return prte_dss_pack_buffer(buffer, src, num_vals, DSS_TYPE_PID_T);
}


/* PACK FUNCTIONS FOR NON-GENERIC SYSTEM TYPES */

/*
 * NULL
 */
int prte_dss_pack_null(prte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prte_data_type_t type)
{
    char null=0x00;
    char *dst;

    PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_pack_null * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = prte_dss_buffer_extend(buffer, num_vals))) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* store the nulls */
    memset(dst, (int)null, num_vals);

    /* update buffer pointers */
    buffer->pack_ptr += num_vals;
    buffer->bytes_used += num_vals;

    return PRTE_SUCCESS;
}

/*
 * BYTE, CHAR, INT8
 */
int prte_dss_pack_byte(prte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prte_data_type_t type)
{
    char *dst;

    PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_pack_byte * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = prte_dss_buffer_extend(buffer, num_vals))) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* store the data */
    memcpy(dst, src, num_vals);

    /* update buffer pointers */
    buffer->pack_ptr += num_vals;
    buffer->bytes_used += num_vals;

    return PRTE_SUCCESS;
}

/*
 * INT16
 */
int prte_dss_pack_int16(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type)
{
    int32_t i;
    uint16_t tmp, *srctmp = (uint16_t*) src;
    char *dst;

    PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_pack_int16 * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = prte_dss_buffer_extend(buffer, num_vals*sizeof(tmp)))) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0; i < num_vals; ++i) {
        tmp = htons(srctmp[i]);
        memcpy(dst, &tmp, sizeof(tmp));
        dst += sizeof(tmp);
    }
    buffer->pack_ptr += num_vals * sizeof(tmp);
    buffer->bytes_used += num_vals * sizeof(tmp);

    return PRTE_SUCCESS;
}

/*
 * INT32
 */
int prte_dss_pack_int32(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type)
{
    int32_t i;
    uint32_t tmp, *srctmp = (uint32_t*) src;
    char *dst;

    PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_pack_int32 * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = prte_dss_buffer_extend(buffer, num_vals*sizeof(tmp)))) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0; i < num_vals; ++i) {
        tmp = htonl(srctmp[i]);
        memcpy(dst, &tmp, sizeof(tmp));
        dst += sizeof(tmp);
    }
    buffer->pack_ptr += num_vals * sizeof(tmp);
    buffer->bytes_used += num_vals * sizeof(tmp);

    return PRTE_SUCCESS;
}

/*
 * INT64
 */
int prte_dss_pack_int64(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type)
{
    int32_t i;
    uint64_t tmp, *srctmp = (uint64_t*) src;
    char *dst;
    size_t bytes_packed = num_vals * sizeof(tmp);

    PRTE_OUTPUT( ( prte_dss_verbose, "prte_dss_pack_int64 * %d\n", num_vals ) );
    /* check to see if buffer needs extending */
    if (NULL == (dst = prte_dss_buffer_extend(buffer, bytes_packed))) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0; i < num_vals; ++i) {
        tmp = prte_hton64(srctmp[i]);
        memcpy(dst, &tmp, sizeof(tmp));
        dst += sizeof(tmp);
    }
    buffer->pack_ptr += bytes_packed;
    buffer->bytes_used += bytes_packed;

    return PRTE_SUCCESS;
}

/*
 * STRING
 */
int prte_dss_pack_string(prte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prte_data_type_t type)
{
    int ret = PRTE_SUCCESS;
    int32_t i, len;
    char **ssrc = (char**) src;

    for (i = 0; i < num_vals; ++i) {
        if (NULL == ssrc[i]) {  /* got zero-length string/NULL pointer - store NULL */
            len = 0;
            if (PRTE_SUCCESS != (ret = prte_dss_pack_int32(buffer, &len, 1, PRTE_INT32))) {
                return ret;
            }
        } else {
            len = (int32_t)strlen(ssrc[i]) + 1;
            if (PRTE_SUCCESS != (ret = prte_dss_pack_int32(buffer, &len, 1, PRTE_INT32))) {
                return ret;
            }
            if (PRTE_SUCCESS != (ret =
                prte_dss_pack_byte(buffer, ssrc[i], len, PRTE_BYTE))) {
                return ret;
            }
        }
    }

    return PRTE_SUCCESS;
}

/* FLOAT */
int prte_dss_pack_float(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type)
{
    int ret = PRTE_SUCCESS;
    int32_t i;
    float *ssrc = (float*)src;
    char *convert;

    for (i = 0; i < num_vals; ++i) {
        prte_asprintf(&convert, "%f", ssrc[i]);
        if (PRTE_SUCCESS != (ret = prte_dss_pack_string(buffer, &convert, 1, PRTE_STRING))) {
            free(convert);
            return ret;
        }
        free(convert);
    }

    return PRTE_SUCCESS;
}

/* DOUBLE */
int prte_dss_pack_double(prte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prte_data_type_t type)
{
    int ret = PRTE_SUCCESS;
    int32_t i;
    double *ssrc = (double*)src;
    char *convert;

    for (i = 0; i < num_vals; ++i) {
        prte_asprintf(&convert, "%f", ssrc[i]);
        if (PRTE_SUCCESS != (ret = prte_dss_pack_string(buffer, &convert, 1, PRTE_STRING))) {
            free(convert);
            return ret;
        }
        free(convert);
    }

    return PRTE_SUCCESS;
}

/* TIMEVAL */
int prte_dss_pack_timeval(prte_buffer_t *buffer, const void *src,
                          int32_t num_vals, prte_data_type_t type)
{
    int64_t tmp[2];
    int ret = PRTE_SUCCESS;
    int32_t i;
    struct timeval *ssrc = (struct timeval *)src;

    for (i = 0; i < num_vals; ++i) {
        tmp[0] = (int64_t)ssrc[i].tv_sec;
        tmp[1] = (int64_t)ssrc[i].tv_usec;
        if (PRTE_SUCCESS != (ret = prte_dss_pack_int64(buffer, tmp, 2, PRTE_INT64))) {
            return ret;
        }
    }

    return PRTE_SUCCESS;
}

/* TIME */
int prte_dss_pack_time(prte_buffer_t *buffer, const void *src,
                       int32_t num_vals, prte_data_type_t type)
{
    int ret = PRTE_SUCCESS;
    int32_t i;
    time_t *ssrc = (time_t *)src;
    uint64_t ui64;

    /* time_t is a system-dependent size, so cast it
     * to uint64_t as a generic safe size
     */
    for (i = 0; i < num_vals; ++i) {
        ui64 = (uint64_t)ssrc[i];
        if (PRTE_SUCCESS != (ret = prte_dss_pack_int64(buffer, &ui64, 1, PRTE_UINT64))) {
            return ret;
        }
    }

    return PRTE_SUCCESS;
}


/* PACK FUNCTIONS FOR GENERIC PRTE TYPES */

/*
 * PRTE_DATA_TYPE
 */
int prte_dss_pack_data_type(prte_buffer_t *buffer, const void *src, int32_t num_vals,
                            prte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, src, num_vals, PRTE_DATA_TYPE_T))) {
    }

    return ret;
}

/*
 * PRTE_BYTE_OBJECT
 */
int prte_dss_pack_byte_object(prte_buffer_t *buffer, const void *src, int32_t num,
                             prte_data_type_t type)
{
    prte_byte_object_t **sbyteptr;
    int32_t i, n;
    int ret;

    sbyteptr = (prte_byte_object_t **) src;

    for (i = 0; i < num; ++i) {
        n = sbyteptr[i]->size;
        if (PRTE_SUCCESS != (ret = prte_dss_pack_int32(buffer, &n, 1, PRTE_INT32))) {
            return ret;
        }
        if (0 < n) {
            if (PRTE_SUCCESS != (ret =
                prte_dss_pack_byte(buffer, sbyteptr[i]->bytes, n, PRTE_BYTE))) {
                return ret;
            }
        }
    }

    return PRTE_SUCCESS;
}

/*
 * PRTE_PSTAT
 */
int prte_dss_pack_pstat(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type)
{
    prte_pstats_t **ptr;
    int32_t i;
    int ret;
    char *cptr;

    ptr = (prte_pstats_t **) src;

    for (i = 0; i < num_vals; ++i) {
        cptr = ptr[i]->node;
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &cptr, 1, PRTE_STRING))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->rank, 1, PRTE_INT32))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->pid, 1, PRTE_PID))) {
            return ret;
        }
        cptr = ptr[i]->cmd;
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &cptr, 1, PRTE_STRING))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->state[0], 1, PRTE_BYTE))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->time, 1, PRTE_TIMEVAL))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->priority, 1, PRTE_INT32))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->num_threads, 1, PRTE_INT16))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->pss, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->vsize, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->rss, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->peak_vsize, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->processor, 1, PRTE_INT16))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->sample_time, 1, PRTE_TIMEVAL))) {
            return ret;
        }
    }

    return PRTE_SUCCESS;
}

static int pack_disk_stats(prte_buffer_t *buffer, prte_diskstats_t *dk)
{
    uint64_t i64;
    int ret;

    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &dk->disk, 1, PRTE_STRING))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_reads_completed;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_reads_merged;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_sectors_read;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->milliseconds_reading;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_writes_completed;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_writes_merged;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_sectors_written;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->milliseconds_writing;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->num_ios_in_progress;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->milliseconds_io;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)dk->weighted_milliseconds_io;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    return PRTE_SUCCESS;
}

static int pack_net_stats(prte_buffer_t *buffer, prte_netstats_t *ns)
{
    uint64_t i64;
    int ret;

    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ns->net_interface, 1, PRTE_STRING))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_bytes_recvd;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_packets_recvd;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_recv_errs;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_bytes_sent;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_packets_sent;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    i64 = (uint64_t)ns->num_send_errs;
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &i64, 1, PRTE_UINT64))) {
        return ret;
    }
    return PRTE_SUCCESS;
}

/*
 * PRTE_NODE_STAT
 */
int prte_dss_pack_node_stat(prte_buffer_t *buffer, const void *src,
                            int32_t num_vals, prte_data_type_t type)
{
    prte_node_stats_t **ptr;
    int32_t i, j;
    int ret;
    prte_diskstats_t *ds;
    prte_netstats_t *ns;

    ptr = (prte_node_stats_t **) src;

    for (i = 0; i < num_vals; ++i) {
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->la, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->la5, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->la15, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->total_mem, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->free_mem, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->buffers, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->cached, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->swap_cached, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->swap_total, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->swap_free, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_float(buffer, &ptr[i]->mapped, 1, PRTE_FLOAT))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->sample_time, 1, PRTE_TIMEVAL))) {
            return ret;
        }
        /* pack the number of disk stat objects on the list */
        j = prte_list_get_size(&ptr[i]->diskstats);
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &j, 1, PRTE_INT32))) {
            return ret;
        }
        if (0 < j) {
            /* pack them */
            PRTE_LIST_FOREACH(ds, &ptr[i]->diskstats, prte_diskstats_t) {
                if (PRTE_SUCCESS != (ret = pack_disk_stats(buffer, ds))) {
                    return ret;
                }
            }
        }
        /* pack the number of net stat objects on the list */
        j = prte_list_get_size(&ptr[i]->netstats);
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &j, 1, PRTE_INT32))) {
            return ret;
        }
        if (0 < j) {
            /* pack them */
            PRTE_LIST_FOREACH(ns, &ptr[i]->netstats, prte_netstats_t) {
                if (PRTE_SUCCESS != (ret = pack_net_stats(buffer, ns))) {
                    return ret;
                }
            }
        }
    }

    return PRTE_SUCCESS;
}

/*
 * PRTE_VALUE
 */
int prte_dss_pack_value(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type)
{
    prte_value_t **ptr;
    int32_t i, n;
    int ret;

    ptr = (prte_value_t **) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack the key and type */
        if (PRTE_SUCCESS != (ret = prte_dss_pack_string(buffer, &ptr[i]->key, 1, PRTE_STRING))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_data_type(buffer, &ptr[i]->type, 1, PRTE_DATA_TYPE))) {
            return ret;
        }
        /* now pack the right field */
        switch (ptr[i]->type) {
        case PRTE_BOOL:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.flag, 1, PRTE_BOOL))) {
                return ret;
            }
            break;
        case PRTE_BYTE:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.byte, 1, PRTE_BYTE))) {
                return ret;
            }
            break;
        case PRTE_STRING:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.string, 1, PRTE_STRING))) {
                return ret;
            }
            break;
        case PRTE_SIZE:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.size, 1, PRTE_SIZE))) {
                return ret;
            }
            break;
        case PRTE_PID:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.pid, 1, PRTE_PID))) {
                return ret;
            }
            break;
        case PRTE_INT:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.integer, 1, PRTE_INT))) {
                return ret;
            }
            break;
        case PRTE_INT8:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.int8, 1, PRTE_INT8))) {
                return ret;
            }
            break;
        case PRTE_INT16:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.int16, 1, PRTE_INT16))) {
                return ret;
            }
            break;
        case PRTE_INT32:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.int32, 1, PRTE_INT32))) {
                return ret;
            }
            break;
        case PRTE_INT64:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.int64, 1, PRTE_INT64))) {
                return ret;
            }
            break;
        case PRTE_UINT:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.uint, 1, PRTE_UINT))) {
                return ret;
            }
            break;
        case PRTE_UINT8:
        case PRTE_PERSIST:
        case PRTE_SCOPE:
        case PRTE_DATA_RANGE:
        case PRTE_PROC_STATE:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.uint8, 1, PRTE_UINT8))) {
                return ret;
            }
            break;
        case PRTE_UINT16:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.uint16, 1, PRTE_UINT16))) {
                return ret;
            }
            break;
        case PRTE_UINT32:
        case PRTE_INFO_DIRECTIVES:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.uint32, 1, PRTE_UINT32))) {
                return ret;
            }
            break;
        case PRTE_UINT64:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.uint64, 1, PRTE_UINT64))) {
                return ret;
            }
            break;
        case PRTE_BYTE_OBJECT:
            /* have to pack by hand so we can match unpack without allocation */
            n = ptr[i]->data.bo.size;
            if (PRTE_SUCCESS != (ret = prte_dss_pack_int32(buffer, &n, 1, PRTE_INT32))) {
                return ret;
            }
            if (0 < n) {
                if (PRTE_SUCCESS != (ret = prte_dss_pack_byte(buffer, ptr[i]->data.bo.bytes, n, PRTE_BYTE))) {
                    return ret;
                }
            }
            break;
        case PRTE_FLOAT:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.fval, 1, PRTE_FLOAT))) {
                return ret;
            }
            break;
        case PRTE_DOUBLE:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.dval, 1, PRTE_DOUBLE))) {
                return ret;
            }
            break;
        case PRTE_TIMEVAL:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.tv, 1, PRTE_TIMEVAL))) {
                return ret;
            }
            break;
        case PRTE_TIME:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.time, 1, PRTE_TIME))) {
                return ret;
            }
            break;
        case PRTE_PTR:
            /* just ignore these values */
            break;
        case PRTE_NAME:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.name, 1, PRTE_NAME))) {
                return ret;
            }
            break;
        case PRTE_STATUS:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.status, 1, PRTE_INT))) {
                return ret;
            }
            break;
        case PRTE_ENVAR:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.envar, 1, PRTE_ENVAR))) {
                return ret;
            }
            break;
        default:
            prte_output(0, "PACK-PRTE-VALUE: UNSUPPORTED TYPE %d FOR KEY %s", (int)ptr[i]->type, ptr[i]->key);
            return PRTE_ERROR;
        }
    }

    return PRTE_SUCCESS;
}


/*
 * BUFFER CONTENTS
 */
int prte_dss_pack_buffer_contents(prte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prte_data_type_t type)
{
    prte_buffer_t **ptr;
    int32_t i;
    int ret;

    ptr = (prte_buffer_t **) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack the number of bytes */
        PRTE_OUTPUT((prte_dss_verbose, "prte_dss_pack_buffer_contents: bytes_used %u\n", (unsigned)ptr[i]->bytes_used));
        if (PRTE_SUCCESS != (ret = prte_dss_pack_sizet(buffer, &ptr[i]->bytes_used, 1, PRTE_SIZE))) {
            return ret;
        }
        /* pack the bytes */
        if (0 < ptr[i]->bytes_used) {
            if (PRTE_SUCCESS != (ret = prte_dss_pack_byte(buffer, ptr[i]->base_ptr, ptr[i]->bytes_used, PRTE_BYTE))) {
                return ret;
            }
        } else {
            ptr[i]->base_ptr = NULL;
        }
    }
    return PRTE_SUCCESS;
}

/*
 * NAME
 */
int prte_dss_pack_name(prte_buffer_t *buffer, const void *src,
                           int32_t num_vals, prte_data_type_t type)
{
    int rc;
    int32_t i;
    prte_process_name_t* proc;
    prte_jobid_t *jobid;
    prte_vpid_t *vpid;

    /* collect all the jobids in a contiguous array */
    jobid = (prte_jobid_t*)malloc(num_vals * sizeof(prte_jobid_t));
    if (NULL == jobid) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    proc = (prte_process_name_t*)src;
    for (i=0; i < num_vals; i++) {
        jobid[i] = proc->jobid;
        proc++;
    }
    /* now pack them in one shot */
    if (PRTE_SUCCESS != (rc =
                         prte_dss_pack_jobid(buffer, jobid, num_vals, PRTE_JOBID))) {
        PRTE_ERROR_LOG(rc);
        free(jobid);
        return rc;
    }
    free(jobid);

    /* collect all the vpids in a contiguous array */
    vpid = (prte_vpid_t*)malloc(num_vals * sizeof(prte_vpid_t));
    if (NULL == vpid) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    proc = (prte_process_name_t*)src;
    for (i=0; i < num_vals; i++) {
        vpid[i] = proc->vpid;
        proc++;
    }
    /* now pack them in one shot */
    if (PRTE_SUCCESS != (rc =
                         prte_dss_pack_vpid(buffer, vpid, num_vals, PRTE_VPID))) {
        PRTE_ERROR_LOG(rc);
        free(vpid);
        return rc;
    }
    free(vpid);

    return PRTE_SUCCESS;
}

/*
 * JOBID
 */
int prte_dss_pack_jobid(prte_buffer_t *buffer, const void *src,
                            int32_t num_vals, prte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    if (PRTE_SUCCESS != (
                         ret = prte_dss_pack_buffer(buffer, src, num_vals, PRTE_JOBID_T))) {
        PRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * VPID
 */
int prte_dss_pack_vpid(prte_buffer_t *buffer, const void *src,
                           int32_t num_vals, prte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    if (PRTE_SUCCESS != (
                         ret = prte_dss_pack_buffer(buffer, src, num_vals, PRTE_VPID_T))) {
        PRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * STATUS
 */
int prte_dss_pack_status(prte_buffer_t *buffer, const void *src,
                         int32_t num_vals, prte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    ret = prte_dss_pack_buffer(buffer, src, num_vals, PRTE_INT);
    if (PRTE_SUCCESS != ret) {
        PRTE_ERROR_LOG(ret);
    }

    return ret;
}

int prte_dss_pack_envar(prte_buffer_t *buffer, const void *src,
                        int32_t num_vals, prte_data_type_t type)
{
    int ret;
    int32_t n;
    prte_envar_t *ptr = (prte_envar_t*)src;

    for (n=0; n < num_vals; n++) {
        if (PRTE_SUCCESS != (ret = prte_dss_pack_string(buffer, &ptr[n].envar, 1, PRTE_STRING))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_string(buffer, &ptr[n].value, 1, PRTE_STRING))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_byte(buffer, &ptr[n].separator, 1, PRTE_BYTE))) {
            return ret;
        }
    }
    return PRTE_SUCCESS;
}
