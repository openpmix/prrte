/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <string.h>

#include "src/util/argv.h"
#include "src/dss/dss.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

/* PRRTE_STD_CNTR */
int prrte_dt_copy_std_cntr(prrte_std_cntr_t **dest, prrte_std_cntr_t *src, prrte_data_type_t type)
{
    prrte_std_cntr_t *val;

    val = (prrte_std_cntr_t*)malloc(sizeof(prrte_std_cntr_t));
    if (NULL == val) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    *val = *src;
    *dest = val;

    return PRRTE_SUCCESS;
}

/**
 * JOB
 */
int prrte_dt_copy_job(prrte_job_t **dest, prrte_job_t *src, prrte_data_type_t type)
{
    (*dest) = src;
    PRRTE_RETAIN(src);

    return PRRTE_SUCCESS;
}

/**
* NODE
 */
int prrte_dt_copy_node(prrte_node_t **dest, prrte_node_t *src, prrte_data_type_t type)
{
    prrte_node_t *node;

    node = PRRTE_NEW(prrte_node_t);
    node->name = strdup(src->name);
    node->state = src->state;
    node->slots = src->slots;
    node->slots_inuse = src->slots_inuse;
    node->slots_max = src->slots_max;
    node->topology = src->topology;
    node->flags = src->flags;
    (*dest) = node;

    return PRRTE_SUCCESS;
}

/**
 * PROC
 */
int prrte_dt_copy_proc(prrte_proc_t **dest, prrte_proc_t *src, prrte_data_type_t type)
{
    (*dest) = src;
    PRRTE_RETAIN(src);
    return PRRTE_SUCCESS;
}

/*
 * APP CONTEXT
 */
int prrte_dt_copy_app_context(prrte_app_context_t **dest, prrte_app_context_t *src, prrte_data_type_t type)
{
    prrte_value_t *kv, *kvnew;

    /* create the new object */
    *dest = PRRTE_NEW(prrte_app_context_t);
    if (NULL == *dest) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    /* copy data into it */
    (*dest)->idx = src->idx;
    if (NULL != src->app) {
        (*dest)->app = strdup(src->app);
    }
    (*dest)->num_procs = src->num_procs;
    (*dest)->argv = prrte_argv_copy(src->argv);
    (*dest)->env = prrte_argv_copy(src->env);
    if (NULL != src->cwd) {
        (*dest)->cwd = strdup(src->cwd);
    }

    PRRTE_LIST_FOREACH(kv, &src->attributes, prrte_value_t) {
        prrte_dss.copy((void**)&kvnew, kv, PRRTE_VALUE);
        prrte_list_append(&(*dest)->attributes, &kvnew->super);
    }

    return PRRTE_SUCCESS;
}

int prrte_dt_copy_proc_state(prrte_proc_state_t **dest, prrte_proc_state_t *src, prrte_data_type_t type)
{
    prrte_proc_state_t *ps;

    ps = (prrte_proc_state_t*)malloc(sizeof(prrte_proc_state_t));
    if (NULL == ps) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    *ps = *src;
    *dest = ps;

    return PRRTE_SUCCESS;
}

int prrte_dt_copy_job_state(prrte_job_state_t **dest, prrte_job_state_t *src, prrte_data_type_t type)
{
    prrte_job_state_t *ps;

    ps = (prrte_job_state_t*)malloc(sizeof(prrte_job_state_t));
    if (NULL == ps) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    *ps = *src;
    *dest = ps;

    return PRRTE_SUCCESS;
}

int prrte_dt_copy_node_state(prrte_node_state_t **dest, prrte_node_state_t *src, prrte_data_type_t type)
{
    prrte_node_state_t *ps;

    ps = (prrte_node_state_t*)malloc(sizeof(prrte_node_state_t));
    if (NULL == ps) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    *ps = *src;
    *dest = ps;

    return PRRTE_SUCCESS;
}

int prrte_dt_copy_exit_code(prrte_exit_code_t **dest, prrte_exit_code_t *src, prrte_data_type_t type)
{
    prrte_exit_code_t *ps;

    ps = (prrte_exit_code_t*)malloc(sizeof(prrte_exit_code_t));
    if (NULL == ps) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    *ps = *src;
    *dest = ps;

    return PRRTE_SUCCESS;
}

/*
 * JOB_MAP
 */
int prrte_dt_copy_map(prrte_job_map_t **dest, prrte_job_map_t *src, prrte_data_type_t type)
{
    prrte_std_cntr_t i;

    if (NULL == src) {
        *dest = NULL;
        return PRRTE_SUCCESS;
    }

    /* create the new object */
    *dest = PRRTE_NEW(prrte_job_map_t);
    if (NULL == *dest) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    /* copy data into it */
    (*dest)->mapping = src->mapping;
    (*dest)->ranking = src->ranking;
    (*dest)->binding = src->binding;
    if (NULL != src->ppr) {
        (*dest)->ppr = strdup(src->ppr);
    }
    (*dest)->display_map = src->display_map;
    (*dest)->num_new_daemons = src->num_new_daemons;
    (*dest)->daemon_vpid_start = src->daemon_vpid_start;
    (*dest)->num_nodes = src->num_nodes;

    /* copy the pointer array - have to do this manually
        * as no dss.copy function is setup for that object
        */
    (*dest)->nodes->lowest_free = src->nodes->lowest_free;
    (*dest)->nodes->number_free = src->nodes->number_free;
    (*dest)->nodes->size = src->nodes->size;
    (*dest)->nodes->max_size = src->nodes->max_size;
    (*dest)->nodes->block_size = src->nodes->block_size;
    for (i=0; i < src->nodes->size; i++) {
        (*dest)->nodes->addr[i] = src->nodes->addr[i];
    }

    return PRRTE_SUCCESS;
}

/*
 * RML tag
 */
int prrte_dt_copy_tag(prrte_rml_tag_t **dest, prrte_rml_tag_t *src, prrte_data_type_t type)
{
    prrte_rml_tag_t *tag;

    if (NULL == src) {
        *dest = NULL;
        return PRRTE_SUCCESS;
    }

    /* create the new space */
    tag = (prrte_rml_tag_t*)malloc(sizeof(prrte_rml_tag_t));
    if (NULL == tag) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    /* copy data into it */
    *tag = *src;
    *dest = tag;

    return PRRTE_SUCCESS;
}

int prrte_dt_copy_daemon_cmd(prrte_daemon_cmd_flag_t **dest, prrte_daemon_cmd_flag_t *src, prrte_data_type_t type)
{
    size_t datasize;

    datasize = sizeof(prrte_daemon_cmd_flag_t);

    *dest = (prrte_daemon_cmd_flag_t*)malloc(datasize);
    if (NULL == *dest) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    memcpy(*dest, src, datasize);

    return PRRTE_SUCCESS;
}

int prrte_dt_copy_iof_tag(prrte_iof_tag_t **dest, prrte_iof_tag_t *src, prrte_data_type_t type)
{
    size_t datasize;

    datasize = sizeof(prrte_iof_tag_t);

    *dest = (prrte_iof_tag_t*)malloc(datasize);
    if (NULL == *dest) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    memcpy(*dest, src, datasize);

    return PRRTE_SUCCESS;
}

int prrte_dt_copy_attr(prrte_attribute_t **dest, prrte_attribute_t *src, prrte_data_type_t type)
{
    *dest = PRRTE_NEW(prrte_attribute_t);
    if (NULL == *dest) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    (*dest)->key = src->key;
    (*dest)->type = src->type;

    memcpy(&(*dest)->data, &src->data, sizeof(src->data));

    return PRRTE_SUCCESS;
}

int prrte_dt_copy_sig(prrte_grpcomm_signature_t **dest, prrte_grpcomm_signature_t *src, prrte_data_type_t type)
{
    *dest = PRRTE_NEW(prrte_grpcomm_signature_t);
    if (NULL == *dest) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    (*dest)->sz = src->sz;
    (*dest)->signature = (prrte_process_name_t*)malloc(src->sz * sizeof(prrte_process_name_t));
    if (NULL == (*dest)->signature) {
        PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
        PRRTE_RELEASE(*dest);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }
    memcpy((*dest)->signature, src->signature, src->sz * sizeof(prrte_process_name_t));
    return PRRTE_SUCCESS;
}
