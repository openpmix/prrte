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

#include "src/util/output.h"
#include "src/dss/dss_internal.h"
#include "src/util/error.h"

int prrte_dss_copy(void **dest, void *src, prrte_data_type_t type)
{
    prrte_dss_type_info_t *info;

    /* check for error */
    if (NULL == dest) {
        return PRRTE_ERR_BAD_PARAM;
    }
    if (NULL == src && (PRRTE_NULL != type && PRRTE_STRING != type)) {
        return PRRTE_ERR_BAD_PARAM;
    }

   /* Lookup the copy function for this type and call it */

    if (NULL == (info = (prrte_dss_type_info_t*)prrte_pointer_array_get_item(&prrte_dss_types, type))) {
        return PRRTE_ERR_UNKNOWN_DATA_TYPE;
    }

    return info->odti_copy_fn(dest, src, type);
}

/*
 * STANDARD COPY FUNCTION - WORKS FOR EVERYTHING NON-STRUCTURED
 */
int prrte_dss_std_copy(void **dest, void *src, prrte_data_type_t type)
{
    size_t datasize;
    uint8_t *val = NULL;

    switch(type) {
    case PRRTE_BOOL:
        datasize = sizeof(bool);
        break;

    case PRRTE_INT:
    case PRRTE_UINT:
    case PRRTE_STATUS:
        datasize = sizeof(int);
        break;

    case PRRTE_SIZE:
        datasize = sizeof(size_t);
        break;

    case PRRTE_PID:
        datasize = sizeof(pid_t);
        break;

    case PRRTE_BYTE:
    case PRRTE_INT8:
    case PRRTE_UINT8:
        datasize = 1;
        break;

    case PRRTE_INT16:
    case PRRTE_UINT16:
        datasize = 2;
        break;

    case PRRTE_INT32:
    case PRRTE_UINT32:
        datasize = 4;
        break;

    case PRRTE_INT64:
    case PRRTE_UINT64:
        datasize = 8;
        break;

    case PRRTE_DATA_TYPE:
        datasize = sizeof(prrte_data_type_t);
        break;

    case PRRTE_FLOAT:
        datasize = sizeof(float);
        break;

    case PRRTE_TIMEVAL:
        datasize = sizeof(struct timeval);
        break;

    case PRRTE_TIME:
        datasize = sizeof(time_t);
        break;

    case PRRTE_NAME:
        datasize = sizeof(prrte_process_name_t);
        break;

    default:
        return PRRTE_ERR_UNKNOWN_DATA_TYPE;
    }

    val = (uint8_t*)malloc(datasize);
    if (NULL == val) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    memcpy(val, src, datasize);
    *dest = val;

    return PRRTE_SUCCESS;
}

/* COPY FUNCTIONS FOR NON-STANDARD SYSTEM TYPES */

/*
 * NULL
 */
int prrte_dss_copy_null(char **dest, char *src, prrte_data_type_t type)
{
    char *val;

    *dest = (char*)malloc(sizeof(char));
    if (NULL == *dest) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    val = *dest;  /* save the address of the value */

    /* set the dest to null */
    *val = 0x00;

    return PRRTE_SUCCESS;
}

/*
 * STRING
 */
int prrte_dss_copy_string(char **dest, char *src, prrte_data_type_t type)
{
    if (NULL == src) {  /* got zero-length string/NULL pointer - store NULL */
        *dest = NULL;
    } else {
        *dest = strdup(src);
    }

    return PRRTE_SUCCESS;
}

/* COPY FUNCTIONS FOR GENERIC PRRTE TYPES */

/*
 * PRRTE_BYTE_OBJECT
 */
int prrte_dss_copy_byte_object(prrte_byte_object_t **dest, prrte_byte_object_t *src,
                              prrte_data_type_t type)
{
    /* allocate space for the new object */
    *dest = (prrte_byte_object_t*)malloc(sizeof(prrte_byte_object_t));
    if (NULL == *dest) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    (*dest)->size = src->size;

    /* allocate the required space for the bytes */
    if (NULL == src->bytes) {
        (*dest)->bytes = NULL;
    } else {
        (*dest)->bytes = (uint8_t*)malloc(src->size);
        if (NULL == (*dest)->bytes) {
            PRRTE_RELEASE(*dest);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        /* copy the data across */
        memcpy((*dest)->bytes, src->bytes, src->size);
    }

    return PRRTE_SUCCESS;
}

/* PRRTE_PSTAT */
int prrte_dss_copy_pstat(prrte_pstats_t **dest, prrte_pstats_t *src,
                        prrte_data_type_t type)
{
    prrte_pstats_t *p;

    /* create the new object */
    *dest = PRRTE_NEW(prrte_pstats_t);
    if (NULL == *dest) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;

    /* copy the individual fields */
    memcpy(p->node, src->node, sizeof(src->node));
    p->rank = src->rank;
    p->pid = src->pid;
    memcpy(p->cmd, src->cmd, sizeof(src->cmd));
    p->state[0] = src->state[0];
    p->time = src->time;
    p->priority = src->priority;
    p->num_threads = src->num_threads;
    p->pss = src->pss;
    p->vsize = src->vsize;
    p->rss = src->rss;
    p->peak_vsize = src->peak_vsize;
    p->processor = src->processor;
    p->sample_time.tv_sec = src->sample_time.tv_sec;
    p->sample_time.tv_usec = src->sample_time.tv_usec;
    return PRRTE_SUCCESS;
}

/* PRRTE_NODE_STAT */
int prrte_dss_copy_node_stat(prrte_node_stats_t **dest, prrte_node_stats_t *src,
                            prrte_data_type_t type)
{
    prrte_node_stats_t *p;

    /* create the new object */
    *dest = PRRTE_NEW(prrte_node_stats_t);
    if (NULL == *dest) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;

    /* copy the individual fields */
    p->la = src->la;
    p->la5 = src->la5;
    p->la15 = src->la15;
    p->total_mem = src->total_mem;
    p->free_mem = src->free_mem;
    p->sample_time.tv_sec = src->sample_time.tv_sec;
    p->sample_time.tv_usec = src->sample_time.tv_usec;
    return PRRTE_SUCCESS;
}

/* PRRTE_VALUE */
int prrte_dss_copy_value(prrte_value_t **dest, prrte_value_t *src,
                        prrte_data_type_t type)
{
    prrte_value_t *p;

    /* create the new object */
    *dest = PRRTE_NEW(prrte_value_t);
    if (NULL == *dest) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    p = *dest;

    /* copy the type and key */
    if (NULL != src->key) {
        p->key = strdup(src->key);
    }
    p->type = src->type;

    /* copy the right field */
    switch (src->type) {
    case PRRTE_BYTE:
        p->data.byte = src->data.byte;
        break;
    case PRRTE_STRING:
        if (NULL != src->data.string) {
            p->data.string = strdup(src->data.string);
        } else {
            p->data.string = NULL;
        }
        break;
    case PRRTE_PID:
        p->data.pid = src->data.pid;
        break;
    case PRRTE_INT:
        /* to avoid alignment issues */
        memcpy(&p->data.integer, &src->data.integer, sizeof(int));
        break;
    case PRRTE_INT8:
        p->data.int8 = src->data.int8;
        break;
    case PRRTE_INT16:
        /* to avoid alignment issues */
        memcpy(&p->data.int16, &src->data.int16, 2);
        break;
    case PRRTE_INT32:
        /* to avoid alignment issues */
        memcpy(&p->data.int32, &src->data.int32, 4);
        break;
    case PRRTE_INT64:
        /* to avoid alignment issues */
        memcpy(&p->data.int64, &src->data.int64, 8);
        break;
    case PRRTE_UINT:
        /* to avoid alignment issues */
        memcpy(&p->data.uint, &src->data.uint, sizeof(unsigned int));
        break;
    case PRRTE_UINT8:
        p->data.uint8 = src->data.uint8;
        break;
    case PRRTE_UINT16:
        /* to avoid alignment issues */
        memcpy(&p->data.uint16, &src->data.uint16, 2);
        break;
    case PRRTE_UINT32:
        /* to avoid alignment issues */
        memcpy(&p->data.uint32, &src->data.uint32, 4);
        break;
    case PRRTE_UINT64:
        /* to avoid alignment issues */
        memcpy(&p->data.uint64, &src->data.uint64, 8);
        break;
    case PRRTE_BYTE_OBJECT:
        if (NULL != src->data.bo.bytes && 0 < src->data.bo.size) {
            p->data.bo.bytes = malloc(src->data.bo.size);
            memcpy(p->data.bo.bytes, src->data.bo.bytes, src->data.bo.size);
            p->data.bo.size = src->data.bo.size;
        } else {
            p->data.bo.bytes = NULL;
            p->data.bo.size = 0;
        }
        break;
    case PRRTE_NAME:
        memcpy(&p->data.name, &src->data.name, sizeof(prrte_process_name_t));
        break;
    case PRRTE_ENVAR:
        PRRTE_CONSTRUCT(&p->data.envar, prrte_envar_t);
        if (NULL != src->data.envar.envar) {
            p->data.envar.envar = strdup(src->data.envar.envar);
        }
        if (NULL != src->data.envar.value) {
            p->data.envar.value = strdup(src->data.envar.value);
        }
        p->data.envar.separator = src->data.envar.separator;
        break;
    default:
        prrte_output(0, "COPY-PRRTE-VALUE: UNSUPPORTED TYPE %d", (int)src->type);
        return PRRTE_ERROR;
    }

    return PRRTE_SUCCESS;
}

int prrte_dss_copy_buffer_contents(prrte_buffer_t **dest, prrte_buffer_t *src,
                                  prrte_data_type_t type)
{
    *dest = PRRTE_NEW(prrte_buffer_t);
    prrte_dss.copy_payload(*dest, src);
    return PRRTE_SUCCESS;
}

/* PROCESS NAME */
int prrte_dss_copy_name(prrte_process_name_t **dest, prrte_process_name_t *src, prrte_data_type_t type)
{
    prrte_process_name_t *val;

    val = (prrte_process_name_t*)malloc(sizeof(prrte_process_name_t));
    if (NULL == val) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    val->jobid = src->jobid;
    val->vpid = src->vpid;

    *dest = val;
    return PRRTE_SUCCESS;
}

/*
 * JOBID
 */
int prrte_dss_copy_jobid(prrte_jobid_t **dest, prrte_jobid_t *src, prrte_data_type_t type)
{
    prrte_jobid_t *val;

    val = (prrte_jobid_t*)malloc(sizeof(prrte_jobid_t));
    if (NULL == val) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    *val = *src;
    *dest = val;

    return PRRTE_SUCCESS;
}

/*
 * VPID
 */
int prrte_dss_copy_vpid(prrte_vpid_t **dest, prrte_vpid_t *src, prrte_data_type_t type)
{
    prrte_vpid_t *val;

    val = (prrte_vpid_t*)malloc(sizeof(prrte_vpid_t));
    if (NULL == val) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    *val = *src;
    *dest = val;

    return PRRTE_SUCCESS;
}

int prrte_dss_copy_envar(prrte_envar_t **dest, prrte_envar_t *src, prrte_data_type_t type)
{
    prrte_envar_t *val;

    val = PRRTE_NEW(prrte_envar_t);
    if (NULL == val) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    if (NULL != src->envar) {
        val->envar = strdup(src->envar);
    }
    if (NULL != src->value) {
        val->value = strdup(src->value);
    }
    val->separator = src->separator;
    *dest = val;

    return PRRTE_SUCCESS;
}
