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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/include/prrte_stdint.h"
#include <stdio.h>

#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/util/printf.h"
#include "src/dss/dss_internal.h"

int prrte_dss_print(char **output, char *prefix, void *src, prrte_data_type_t type)
{
    prrte_dss_type_info_t *info;

    /* check for error */
    if (NULL == output) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* Lookup the print function for this type and call it */

    if(NULL == (info = (prrte_dss_type_info_t*)prrte_pointer_array_get_item(&prrte_dss_types, type))) {
        return PRRTE_ERR_UNKNOWN_DATA_TYPE;
    }

    return info->odti_print_fn(output, prefix, src, type);
}

/*
 * STANDARD PRINT FUNCTIONS FOR SYSTEM TYPES
 */
int prrte_dss_print_byte(char **output, char *prefix, uint8_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_BYTE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_BYTE\tValue: %x", prefix, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_string(char **output, char *prefix, char *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_STRING\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_STRING\tValue: %s", prefx, src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_size(char **output, char *prefix, size_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_SIZE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_SIZE\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_pid(char **output, char *prefix, pid_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_PID\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_PID\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }
    return PRRTE_SUCCESS;
}

int prrte_dss_print_bool(char **output, char *prefix, bool *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_BOOL\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_BOOL\tValue: %s", prefx, *src ? "TRUE" : "FALSE");
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_int(char **output, char *prefix, int *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_INT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_INT\tValue: %ld", prefx, (long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_uint(char **output, char *prefix, int *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_UINT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_UINT\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_uint8(char **output, char *prefix, uint8_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_UINT8\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_UINT8\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_uint16(char **output, char *prefix, uint16_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_UINT16\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_UINT16\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_uint32(char **output, char *prefix, uint32_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_UINT32\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_UINT32\tValue: %u", prefx, (unsigned int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_int8(char **output, char *prefix, int8_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_INT8\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_INT8\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_int16(char **output, char *prefix, int16_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_INT16\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_INT16\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_int32(char **output, char *prefix, int32_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_INT32\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_INT32\tValue: %d", prefx, (int) *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}
int prrte_dss_print_uint64(char **output, char *prefix,
#ifdef HAVE_INT64_T
                          uint64_t *src,
#else
                          void *src,
#endif  /* HAVE_INT64_T */
                          prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_UINT64\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

#ifdef HAVE_INT64_T
    prrte_asprintf(output, "%sData type: PRRTE_UINT64\tValue: %lu", prefx, (unsigned long) *src);
#else
    prrte_asprintf(output, "%sData type: PRRTE_UINT64\tValue: unsupported", prefx);
#endif  /* HAVE_INT64_T */
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_int64(char **output, char *prefix,
#ifdef HAVE_INT64_T
                         int64_t *src,
#else
                         void *src,
#endif  /* HAVE_INT64_T */
                         prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_INT64\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

#ifdef HAVE_INT64_T
    prrte_asprintf(output, "%sData type: PRRTE_INT64\tValue: %ld", prefx, (long) *src);
#else
    prrte_asprintf(output, "%sData type: PRRTE_INT64\tValue: unsupported", prefx);
#endif  /* HAVE_INT64_T */
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_float(char **output, char *prefix,
                         float *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_FLOAT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_FLOAT\tValue: %f", prefx, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_double(char **output, char *prefix,
                          double *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_DOUBLE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_DOUBLE\tValue: %f", prefx, *src);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_time(char **output, char *prefix,
                        time_t *src, prrte_data_type_t type)
{
    char *prefx;
    char *t;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_TIME\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    t = ctime(src);
    t[strlen(t)-1] = '\0';  // remove trailing newline

    prrte_asprintf(output, "%sData type: PRRTE_TIME\tValue: %s", prefx, t);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_timeval(char **output, char *prefix,
                           struct timeval *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_TIMEVAL\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_TIMEVAL\tValue: %ld.%06ld", prefx,
             (long)src->tv_sec, (long)src->tv_usec);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_null(char **output, char *prefix, void *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_NULL\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_NULL", prefx);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}


/* PRINT FUNCTIONS FOR GENERIC PRRTE TYPES */

/*
 * PRRTE_DATA_TYPE
 */
int prrte_dss_print_data_type(char **output, char *prefix, prrte_data_type_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_DATA_TYPE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_DATA_TYPE\tValue: %lu", prefx, (unsigned long) *src);
    if (prefx != prefix) {
        free(prefx);
    }
    return PRRTE_SUCCESS;
}

/*
 * PRRTE_BYTE_OBJECT
 */
int prrte_dss_print_byte_object(char **output, char *prefix, prrte_byte_object_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_BYTE_OBJECT\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_BYTE_OBJECT\tSize: %lu", prefx, (unsigned long) src->size);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

/*
 * PRRTE_PSTAT
 */
int prrte_dss_print_pstat(char **output, char *prefix, prrte_pstats_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_PSTATS\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }
    prrte_asprintf(output, "%sPRRTE_PSTATS SAMPLED AT: %ld.%06ld\n%snode: %s rank: %d pid: %d cmd: %s state: %c pri: %d #threads: %d Processor: %d\n"
             "%s\ttime: %ld.%06ld cpu: %5.2f  PSS: %8.2f  VMsize: %8.2f PeakVMSize: %8.2f RSS: %8.2f\n",
             prefx, (long)src->sample_time.tv_sec, (long)src->sample_time.tv_usec,
             prefx, src->node, src->rank, src->pid, src->cmd, src->state[0], src->priority, src->num_threads, src->processor,
             prefx, (long)src->time.tv_sec, (long)src->time.tv_usec, src->percent_cpu, src->pss, src->vsize, src->peak_vsize, src->rss);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

/*
 * PRRTE_NODE_STAT
 */
int prrte_dss_print_node_stat(char **output, char *prefix, prrte_node_stats_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_NODE_STATS\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }
    prrte_asprintf(output, "%sPRRTE_NODE_STATS SAMPLED AT: %ld.%06ld\n%sTotal Mem: %5.2f Free Mem: %5.2f Buffers: %5.2f Cached: %5.2f\n"
             "%sSwapCached: %5.2f SwapTotal: %5.2f SwapFree: %5.2f Mapped: %5.2f\n"
             "%s\tla: %5.2f\tla5: %5.2f\tla15: %5.2f\n",
             prefx, (long)src->sample_time.tv_sec, (long)src->sample_time.tv_usec,
             prefx, src->total_mem, src->free_mem, src->buffers, src->cached,
             prefx, src->swap_cached, src->swap_total, src->swap_free, src->mapped,
             prefx, src->la, src->la5, src->la15);
    if (prefx != prefix) {
        free(prefx);
    }

    return PRRTE_SUCCESS;
}

/*
 * PRRTE_VALUE
 */
int prrte_dss_print_value(char **output, char *prefix, prrte_value_t *src, prrte_data_type_t type)
{
    char *prefx;

    /* deal with NULL prefix */
    if (NULL == prefix) prrte_asprintf(&prefx, " ");
    else prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_VALUE\tValue: NULL pointer", prefx);
        if (prefx != prefix) {
            free(prefx);
        }
        return PRRTE_SUCCESS;
    }

    switch (src->type) {
    case PRRTE_BOOL:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_BOOL\tKey: %s\tValue: %s",
                 prefx, src->key, src->data.flag ? "true" : "false");
        break;
    case PRRTE_BYTE:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_BYTE\tKey: %s\tValue: %x",
                 prefx, src->key, src->data.byte);
        break;
    case PRRTE_STRING:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_STRING\tKey: %s\tValue: %s",
                 prefx, src->key, src->data.string);
        break;
    case PRRTE_SIZE:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_SIZE\tKey: %s\tValue: %lu",
                 prefx, src->key, (unsigned long)src->data.size);
        break;
    case PRRTE_PID:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_PID\tKey: %s\tValue: %lu",
                 prefx, src->key, (unsigned long)src->data.pid);
        break;
    case PRRTE_INT:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_INT\tKey: %s\tValue: %d",
                 prefx, src->key, src->data.integer);
        break;
    case PRRTE_INT8:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_INT8\tKey: %s\tValue: %d",
                 prefx, src->key, (int)src->data.int8);
        break;
    case PRRTE_INT16:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_INT16\tKey: %s\tValue: %d",
                 prefx, src->key, (int)src->data.int16);
        break;
    case PRRTE_INT32:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_INT32\tKey: %s\tValue: %d",
                 prefx, src->key, src->data.int32);
        break;
    case PRRTE_INT64:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_INT64\tKey: %s\tValue: %ld",
                 prefx, src->key, (long)src->data.int64);
        break;
    case PRRTE_UINT:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_UINT\tKey: %s\tValue: %u",
                 prefx, src->key, src->data.uint);
        break;
    case PRRTE_UINT8:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_UINT8\tKey: %s\tValue: %u",
                 prefx, src->key, (unsigned int)src->data.uint8);
        break;
    case PRRTE_UINT16:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_UINT16\tKey: %s\tValue: %u",
                 prefx, src->key, (unsigned int)src->data.uint16);
        break;
    case PRRTE_UINT32:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_UINT32\tKey: %s\tValue: %u",
                 prefx, src->key, src->data.uint32);
        break;
    case PRRTE_UINT64:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_UINT64\tKey: %s\tValue: %lu",
                 prefx, src->key, (unsigned long)src->data.uint64);
        break;
    case PRRTE_FLOAT:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_FLOAT\tKey: %s\tValue: %f",
                 prefx, src->key, src->data.fval);
        break;
    case PRRTE_DOUBLE:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_DOUBLE\tKey: %s\tValue: %f",
                 prefx, src->key, src->data.dval);
        break;
    case PRRTE_BYTE_OBJECT:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_BYTE_OBJECT\tKey: %s\tData: %s\tSize: %lu",
                 prefx, src->key, (NULL == src->data.bo.bytes) ? "NULL" : "NON-NULL", (unsigned long)src->data.bo.size);
        break;
    case PRRTE_TIMEVAL:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_TIMEVAL\tKey: %s\tValue: %ld.%06ld", prefx,
                 src->key, (long)src->data.tv.tv_sec, (long)src->data.tv.tv_usec);
        break;
    case PRRTE_TIME:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_TIME\tKey: %s\tValue: %s", prefx,
                 src->key, ctime(&src->data.time));
        break;
    case PRRTE_NAME:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_NAME\tKey: %s\tValue: %s", prefx,
                 src->key, PRRTE_NAME_PRINT(&src->data.name));
        break;
    case PRRTE_PTR:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_PTR\tKey: %s", prefx, src->key);
        break;
    case PRRTE_ENVAR:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_ENVAR\tKey: %s\tName: %s\tValue: %s\tSeparator: %c",
                 prefx, src->key,
                 (NULL == src->data.envar.envar) ? "NULL" : src->data.envar.envar,
                 (NULL == src->data.envar.value) ? "NULL" : src->data.envar.value,
                 ('\0' == src->data.envar.separator) ? ' ' : src->data.envar.separator);
        break;
    default:
        prrte_asprintf(output, "%sPRRTE_VALUE: Data type: UNKNOWN\tKey: %s\tValue: UNPRINTABLE",
                 prefx, src->key);
        break;
    }
    if (prefx != prefix) {
        free(prefx);
    }
    return PRRTE_SUCCESS;
}

int prrte_dss_print_buffer_contents(char **output, char *prefix,
                                   prrte_buffer_t *src, prrte_data_type_t type)
{
    return PRRTE_SUCCESS;
}

/*
 * NAME
 */
int prrte_dss_print_name(char **output, char *prefix, prrte_process_name_t *name, prrte_data_type_t type)
{
    /* set default result */
    *output = NULL;

    if (NULL == name) {
        prrte_asprintf(output, "%sData type: PRRTE_PROCESS_NAME\tData Value: NULL",
                 (NULL == prefix ? " " : prefix));
    } else {
        prrte_asprintf(output, "%sData type: PRRTE_PROCESS_NAME\tData Value: [%d,%d]",
                 (NULL == prefix ? " " : prefix), name->jobid, name->vpid);
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_print_jobid(char **output, char *prefix,
                         prrte_process_name_t *src, prrte_data_type_t type)
{
    char *prefx = " ";

    /* deal with NULL prefix */
    if (NULL != prefix) prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_JOBID\tValue: NULL pointer", prefx);
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_JOBID\tValue: %s", prefx, PRRTE_JOBID_PRINT(src->jobid));
    return PRRTE_SUCCESS;
}

int prrte_dss_print_vpid(char **output, char *prefix,
                         prrte_process_name_t *src, prrte_data_type_t type)
{
    char *prefx = " ";

    /* deal with NULL prefix */
    if (NULL != prefix) prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_VPID\tValue: NULL pointer", prefx);
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_VPID\tValue: %s", prefx, PRRTE_VPID_PRINT(src->vpid));
    return PRRTE_SUCCESS;
}

int prrte_dss_print_status(char **output, char *prefix,
                          int *src, prrte_data_type_t type)
{
    char *prefx = " ";

    /* deal with NULL prefix */
    if (NULL != prefix) prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_STATUS\tValue: NULL pointer", prefx);
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sData type: PRRTE_STATUS\tValue: %s", prefx, prrte_strerror(*src));
    return PRRTE_SUCCESS;
}


int prrte_dss_print_envar(char **output, char *prefix,
                         prrte_envar_t *src, prrte_data_type_t type)
{
    char *prefx = " ";

    /* deal with NULL prefix */
    if (NULL != prefix) prefx = prefix;

    /* if src is NULL, just print data type and return */
    if (NULL == src) {
        prrte_asprintf(output, "%sData type: PRRTE_ENVAR\tValue: NULL pointer", prefx);
        return PRRTE_SUCCESS;
    }

    prrte_asprintf(output, "%sPRRTE_VALUE: Data type: PRRTE_ENVAR\tName: %s\tValue: %s\tSeparator: %c",
             prefx, (NULL == src->envar) ? "NULL" : src->envar,
             (NULL == src->value) ? "NULL" : src->value,
             ('\0' == src->separator) ? ' ' : src->separator);
    return PRRTE_SUCCESS;
}
