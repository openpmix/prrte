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
 * Copyright (c) 2012      Los Alamos National Security, Inc.  All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/include/prte_stdint.h"
#include <stdio.h>

#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/util/printf.h"
#include "src/dss/dss_internal.h"

int prte_dss_print(char **output, char *prefix, void *src, prte_data_type_t type)
{
    prte_dss_type_info_t *info;

    /* check for error */
    if (NULL == output) {
        return PRTE_ERR_BAD_PARAM;
    }

    /* Lookup the print function for this type and call it */

    if(NULL == (info = (prte_dss_type_info_t*)prte_pointer_array_get_item(&prte_dss_types, type))) {
        return PRTE_ERR_UNKNOWN_DATA_TYPE;
    }

    return info->odti_print_fn(output, prefix, src, type);
}

/*
 * STANDARD PRINT FUNCTIONS FOR SYSTEM TYPES
 */
int prte_dss_print_byte(char **output, char *prefix, uint8_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_BYTE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_BYTE\tValue: %x", prefix, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_string(char **output, char *prefix, char *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_STRING\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_STRING\tValue: %s", prefx, src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_size(char **output, char *prefix, size_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_SIZE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_SIZE\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_pid(char **output, char *prefix, pid_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_PID\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_PID\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }
    return PRTE_SUCCESS;
}

int prte_dss_print_bool(char **output, char *prefix, bool *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_BOOL\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_BOOL\tValue: %s", prefx, *src ? "TRUE" : "FALSE");
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_int(char **output, char *prefix, int *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_INT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_INT\tValue: %ld", prefx, (long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_uint(char **output, char *prefix, int *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_UINT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_UINT\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_uint8(char **output, char *prefix, uint8_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_UINT8\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_UINT8\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_uint16(char **output, char *prefix, uint16_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_UINT16\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_UINT16\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_uint32(char **output, char *prefix, uint32_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_UINT32\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_UINT32\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_int8(char **output, char *prefix, int8_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_INT8\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_INT8\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_int16(char **output, char *prefix, int16_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_INT16\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_INT16\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_int32(char **output, char *prefix, int32_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_INT32\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_INT32\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}
int prte_dss_print_uint64(char **output, char *prefix,
#ifdef HAVE_INT64_T
                          uint64_t *src,
#else
                          void *src,
#endif  /* HAVE_INT64_T */
                          prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_UINT64\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

#ifdef HAVE_INT64_T
    prte_asprintf(output, "%sData type: PRTE_UINT64\tValue: %lu", prefx, (unsigned long) *src);
#else
    prte_asprintf(output, "%sData type: PRTE_UINT64\tValue: unsupported", prefx);
#endif  /* HAVE_INT64_T */
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_int64(char **output, char *prefix,
#ifdef HAVE_INT64_T
                         int64_t *src,
#else
                         void *src,
#endif  /* HAVE_INT64_T */
                         prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_INT64\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

#ifdef HAVE_INT64_T
    prte_asprintf(output, "%sData type: PRTE_INT64\tValue: %ld", prefx, (long) *src);
#else
    prte_asprintf(output, "%sData type: PRTE_INT64\tValue: unsupported", prefx);
#endif  /* HAVE_INT64_T */
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_float(char **output, char *prefix,
                         float *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_FLOAT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_FLOAT\tValue: %f", prefx, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_double(char **output, char *prefix,
                          double *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_DOUBLE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_DOUBLE\tValue: %f", prefx, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_time(char **output, char *prefix,
                        time_t *src, prte_data_type_t type)
{
    char *prefx;
    char *t;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_TIME\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    t = ctime(src);
    t[strlen(t)-1] = '\0';  // remove trailing newline

    prte_asprintf(output, "%sData type: PRTE_TIME\tValue: %s", prefx, t);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_timeval(char **output, char *prefix,
                           struct timeval *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_TIMEVAL\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_TIMEVAL\tValue: %ld.%06ld", prefx,
             (long)src->tv_sec, (long)src->tv_usec);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_null(char **output, char *prefix, void *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_NULL\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_NULL", prefx);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}


/* PRINT FUNCTIONS FOR GENERIC PRTE TYPES */

/*
 * PRTE_DATA_TYPE
 */
int prte_dss_print_data_type(char **output, char *prefix, prte_data_type_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_DATA_TYPE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_DATA_TYPE\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }
    return PRTE_SUCCESS;
}

/*
 * PRTE_BYTE_OBJECT
 */
int prte_dss_print_byte_object(char **output, char *prefix, prte_byte_object_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_BYTE_OBJECT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_BYTE_OBJECT\tSize: %lu", prefx, (unsigned long) src->size);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

/*
 * PRTE_PSTAT
 */
int prte_dss_print_pstat(char **output, char *prefix, prte_pstats_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_PSTATS\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }
    prte_asprintf(output, "%sPRTE_PSTATS SAMPLED AT: %ld.%06ld\n%snode: %s rank: %d pid: %d cmd: %s state: %c pri: %d #threads: %d Processor: %d\n"
             "%s\ttime: %ld.%06ld cpu: %5.2f  PSS: %8.2f  VMsize: %8.2f PeakVMSize: %8.2f RSS: %8.2f\n",
             prefx, (long)src->sample_time.tv_sec, (long)src->sample_time.tv_usec,
             prefx, src->node, src->rank, src->pid, src->cmd, src->state[0], src->priority, src->num_threads, src->processor,
             prefx, (long)src->time.tv_sec, (long)src->time.tv_usec, src->percent_cpu, src->pss, src->vsize, src->peak_vsize, src->rss);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

/*
 * PRTE_NODE_STAT
 */
int prte_dss_print_node_stat(char **output, char *prefix, prte_node_stats_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_NODE_STATS\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }
    prte_asprintf(output, "%sPRTE_NODE_STATS SAMPLED AT: %ld.%06ld\n%sTotal Mem: %5.2f Free Mem: %5.2f Buffers: %5.2f Cached: %5.2f\n"
             "%sSwapCached: %5.2f SwapTotal: %5.2f SwapFree: %5.2f Mapped: %5.2f\n"
             "%s\tla: %5.2f\tla5: %5.2f\tla15: %5.2f\n",
             prefx, (long)src->sample_time.tv_sec, (long)src->sample_time.tv_usec,
             prefx, src->total_mem, src->free_mem, src->buffers, src->cached,
             prefx, src->swap_cached, src->swap_total, src->swap_free, src->mapped,
             prefx, src->la, src->la5, src->la15);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRTE_SUCCESS;
}

/*
 * PRTE_VALUE
 */
int prte_dss_print_value(char **output, char *prefix, prte_value_t *src, prte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_VALUE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRTE_SUCCESS;
    }

    switch (src->type) {
    case PRTE_BOOL:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_BOOL\tKey: %s\tValue: %s",
                 prefx, src->key, src->data.flag ? "true" : "false");
        break;
    case PRTE_BYTE:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_BYTE\tKey: %s\tValue: %x",
                 prefx, src->key, src->data.byte);
        break;
    case PRTE_STRING:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_STRING\tKey: %s\tValue: %s",
                 prefx, src->key, src->data.string);
        break;
    case PRTE_SIZE:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_SIZE\tKey: %s\tValue: %lu",
                 prefx, src->key, (unsigned long)src->data.size);
        break;
    case PRTE_PID:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_PID\tKey: %s\tValue: %lu",
                 prefx, src->key, (unsigned long)src->data.pid);
        break;
    case PRTE_INT:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_INT\tKey: %s\tValue: %d",
                 prefx, src->key, src->data.integer);
        break;
    case PRTE_INT8:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_INT8\tKey: %s\tValue: %d",
                 prefx, src->key, (int)src->data.int8);
        break;
    case PRTE_INT16:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_INT16\tKey: %s\tValue: %d",
                 prefx, src->key, (int)src->data.int16);
        break;
    case PRTE_INT32:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_INT32\tKey: %s\tValue: %d",
                 prefx, src->key, src->data.int32);
        break;
    case PRTE_INT64:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_INT64\tKey: %s\tValue: %ld",
                 prefx, src->key, (long)src->data.int64);
        break;
    case PRTE_UINT:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_UINT\tKey: %s\tValue: %u",
                 prefx, src->key, src->data.uint);
        break;
    case PRTE_UINT8:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_UINT8\tKey: %s\tValue: %u",
                 prefx, src->key, (unsigned int)src->data.uint8);
        break;
    case PRTE_UINT16:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_UINT16\tKey: %s\tValue: %u",
                 prefx, src->key, (unsigned int)src->data.uint16);
        break;
    case PRTE_UINT32:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_UINT32\tKey: %s\tValue: %u",
                 prefx, src->key, src->data.uint32);
        break;
    case PRTE_UINT64:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_UINT64\tKey: %s\tValue: %lu",
                 prefx, src->key, (unsigned long)src->data.uint64);
        break;
    case PRTE_FLOAT:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_FLOAT\tKey: %s\tValue: %f",
                 prefx, src->key, src->data.fval);
        break;
    case PRTE_DOUBLE:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_DOUBLE\tKey: %s\tValue: %f",
                 prefx, src->key, src->data.dval);
        break;
    case PRTE_BYTE_OBJECT:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_BYTE_OBJECT\tKey: %s\tData: %s\tSize: %lu",
                 prefx, src->key, (NULL == src->data.bo.bytes) ? "NULL" : "NON-NULL", (unsigned long)src->data.bo.size);
        break;
    case PRTE_TIMEVAL:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_TIMEVAL\tKey: %s\tValue: %ld.%06ld", prefx,
                 src->key, (long)src->data.tv.tv_sec, (long)src->data.tv.tv_usec);
        break;
    case PRTE_TIME:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_TIME\tKey: %s\tValue: %s", prefx,
                 src->key, ctime(&src->data.time));
        break;
    case PRTE_NAME:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_NAME\tKey: %s\tValue: %s", prefx,
                 src->key, PRTE_NAME_PRINT(&src->data.name));
        break;
    case PRTE_PTR:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_PTR\tKey: %s", prefx, src->key);
        break;
    case PRTE_ENVAR:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_ENVAR\tKey: %s\tName: %s\tValue: %s\tSeparator: %c",
                 prefx, src->key,
                 (NULL == src->data.envar.envar) ? "NULL" : src->data.envar.envar,
                 (NULL == src->data.envar.value) ? "NULL" : src->data.envar.value,
                 ('\0' == src->data.envar.separator) ? ' ' : src->data.envar.separator);
        break;
    default:
        prte_asprintf(output, "%sPRTE_VALUE: Data type: UNKNOWN\tKey: %s\tValue: UNPRINTABLE",
                 prefx, src->key);
        break;
    }
    if (prefx != prefix) {
        free(prefx);
    }
    return PRTE_SUCCESS;
}

int prte_dss_print_buffer_contents(char **output, char *prefix,
                                   prte_buffer_t *src, prte_data_type_t type)
{
    return PRTE_SUCCESS;
}

/*
 * NAME
 */
int prte_dss_print_name(char **output, char *prefix, prte_process_name_t *name, prte_data_type_t type)
{
    /* set default result */
    *output = NULL;

    if (NULL == name) {
        prte_asprintf(output, "%sData type: PRTE_PROCESS_NAME\tData Value: NULL",
                 (NULL == prefix ? " " : prefix));
    } else {
        prte_asprintf(output, "%sData type: PRTE_PROCESS_NAME\tData Value: [%d,%d]",
                 (NULL == prefix ? " " : prefix), name->jobid, name->vpid);
    }

    return PRTE_SUCCESS;
}

int prte_dss_print_jobid(char **output, char *prefix,
                         prte_process_name_t *src, prte_data_type_t type)
{
    char *prefx = " ";

    /* deal with NULL prefix */
    if (NULL != prefix) prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_JOBID\tValue: NULL pointer", prefx);
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_JOBID\tValue: %s", prefx, PRTE_JOBID_PRINT(src->jobid));
    return PRTE_SUCCESS;
}

int prte_dss_print_vpid(char **output, char *prefix,
                         prte_process_name_t *src, prte_data_type_t type)
{
    char *prefx = " ";

    /* deal with NULL prefix */
    if (NULL != prefix) prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_VPID\tValue: NULL pointer", prefx);
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_VPID\tValue: %s", prefx, PRTE_VPID_PRINT(src->vpid));
    return PRTE_SUCCESS;
}

int prte_dss_print_status(char **output, char *prefix,
                          int *src, prte_data_type_t type)
{
    char *prefx = " ";

    /* deal with NULL prefix */
    if (NULL != prefix) prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_STATUS\tValue: NULL pointer", prefx);
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sData type: PRTE_STATUS\tValue: %s", prefx, prte_strerror(*src));
    return PRTE_SUCCESS;
}


int prte_dss_print_envar(char **output, char *prefix,
                         prte_envar_t *src, prte_data_type_t type)
{
    char *prefx = " ";

    /* deal with NULL prefix */
    if (NULL != prefix) prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prte_asprintf(output, "%sData type: PRTE_ENVAR\tValue: NULL pointer", prefx);
        return PRTE_SUCCESS;
    }

    prte_asprintf(output, "%sPRTE_VALUE: Data type: PRTE_ENVAR\tName: %s\tValue: %s\tSeparator: %c",
             prefx, (NULL == src->envar) ? "NULL" : src->envar,
             (NULL == src->value) ? "NULL" : src->value,
             ('\0' == src->separator) ? ' ' : src->separator);
    return PRTE_SUCCESS;
}
