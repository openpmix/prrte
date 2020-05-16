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
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <string.h>

#include "src/util/argv.h"
#include "src/dss/dss.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/data_type_support/prte_dt_support.h"

/* PRTE_STD_CNTR */
int prte_dt_copy_std_cntr(prte_std_cntr_t **dest, prte_std_cntr_t *src, prte_data_type_t type)
{
    prte_std_cntr_t *val;

    val = (prte_std_cntr_t*)malloc(sizeof(prte_std_cntr_t));
    if (NULL == val) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    *val = *src;
    *dest = val;

    return PRTE_SUCCESS;
}

/**
 * JOB
 */
int prte_dt_copy_job(prte_job_t **dest, prte_job_t *src, prte_data_type_t type)
{
    (*dest) = src;
    PRTE_RETAIN(src);

    return PRTE_SUCCESS;
}

/**
* NODE
 */
int prte_dt_copy_node(prte_node_t **dest, prte_node_t *src, prte_data_type_t type)
{
    prte_node_t *node;

    node = PRTE_NEW(prte_node_t);
    node->name = strdup(src->name);
    node->state = src->state;
    node->slots = src->slots;
    node->slots_inuse = src->slots_inuse;
    node->slots_max = src->slots_max;
    node->topology = src->topology;
    node->flags = src->flags;
    (*dest) = node;

    return PRTE_SUCCESS;
}

/**
 * PROC
 */
int prte_dt_copy_proc(prte_proc_t **dest, prte_proc_t *src, prte_data_type_t type)
{
    (*dest) = src;
    PRTE_RETAIN(src);
    return PRTE_SUCCESS;
}

/*
 * APP CONTEXT
 */
int prte_dt_copy_app_context(prte_app_context_t **dest, prte_app_context_t *src, prte_data_type_t type)
{
    prte_value_t *kv, *kvnew;

    /* create the new object */
    *dest = PRTE_NEW(prte_app_context_t);
    if (NULL == *dest) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* copy data into it */
    (*dest)->idx = src->idx;
    if (NULL != src->app) {
        (*dest)->app = strdup(src->app);
    }
    (*dest)->num_procs = src->num_procs;
    (*dest)->argv = prte_argv_copy(src->argv);
    (*dest)->env = prte_argv_copy(src->env);
    if (NULL != src->cwd) {
        (*dest)->cwd = strdup(src->cwd);
    }

    PRTE_LIST_FOREACH(kv, &src->attributes, prte_value_t) {
        prte_dss.copy((void**)&kvnew, kv, PRTE_VALUE);
        prte_list_append(&(*dest)->attributes, &kvnew->super);
    }

    return PRTE_SUCCESS;
}

int prte_dt_copy_proc_state(prte_proc_state_t **dest, prte_proc_state_t *src, prte_data_type_t type)
{
    prte_proc_state_t *ps;

    ps = (prte_proc_state_t*)malloc(sizeof(prte_proc_state_t));
    if (NULL == ps) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    *ps = *src;
    *dest = ps;

    return PRTE_SUCCESS;
}

int prte_dt_copy_job_state(prte_job_state_t **dest, prte_job_state_t *src, prte_data_type_t type)
{
    prte_job_state_t *ps;

    ps = (prte_job_state_t*)malloc(sizeof(prte_job_state_t));
    if (NULL == ps) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    *ps = *src;
    *dest = ps;

    return PRTE_SUCCESS;
}

int prte_dt_copy_node_state(prte_node_state_t **dest, prte_node_state_t *src, prte_data_type_t type)
{
    prte_node_state_t *ps;

    ps = (prte_node_state_t*)malloc(sizeof(prte_node_state_t));
    if (NULL == ps) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    *ps = *src;
    *dest = ps;

    return PRTE_SUCCESS;
}

int prte_dt_copy_exit_code(prte_exit_code_t **dest, prte_exit_code_t *src, prte_data_type_t type)
{
    prte_exit_code_t *ps;

    ps = (prte_exit_code_t*)malloc(sizeof(prte_exit_code_t));
    if (NULL == ps) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    *ps = *src;
    *dest = ps;

    return PRTE_SUCCESS;
}

/*
 * JOB_MAP
 */
int prte_dt_copy_map(prte_job_map_t **dest, prte_job_map_t *src, prte_data_type_t type)
{
    prte_std_cntr_t i;

    if (NULL == src) {
        *dest = NULL;
        return PRTE_SUCCESS;
    }

    /* create the new object */
    *dest = PRTE_NEW(prte_job_map_t);
    if (NULL == *dest) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* copy data into it */
    (*dest)->mapping = src->mapping;
    (*dest)->ranking = src->ranking;
    (*dest)->binding = src->binding;
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

    return PRTE_SUCCESS;
}

/*
 * RML tag
 */
int prte_dt_copy_tag(prte_rml_tag_t **dest, prte_rml_tag_t *src, prte_data_type_t type)
{
    prte_rml_tag_t *tag;

    if (NULL == src) {
        *dest = NULL;
        return PRTE_SUCCESS;
    }

    /* create the new space */
    tag = (prte_rml_tag_t*)malloc(sizeof(prte_rml_tag_t));
    if (NULL == tag) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* copy data into it */
    *tag = *src;
    *dest = tag;

    return PRTE_SUCCESS;
}

int prte_dt_copy_daemon_cmd(prte_daemon_cmd_flag_t **dest, prte_daemon_cmd_flag_t *src, prte_data_type_t type)
{
    size_t datasize;

    datasize = sizeof(prte_daemon_cmd_flag_t);

    *dest = (prte_daemon_cmd_flag_t*)malloc(datasize);
    if (NULL == *dest) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    memcpy(*dest, src, datasize);

    return PRTE_SUCCESS;
}

int prte_dt_copy_iof_tag(prte_iof_tag_t **dest, prte_iof_tag_t *src, prte_data_type_t type)
{
    size_t datasize;

    datasize = sizeof(prte_iof_tag_t);

    *dest = (prte_iof_tag_t*)malloc(datasize);
    if (NULL == *dest) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    memcpy(*dest, src, datasize);

    return PRTE_SUCCESS;
}

int prte_dt_copy_attr(prte_attribute_t **dest, prte_attribute_t *src, prte_data_type_t type)
{
    *dest = PRTE_NEW(prte_attribute_t);
    if (NULL == *dest) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    (*dest)->key = src->key;
    (*dest)->type = src->type;

    memcpy(&(*dest)->data, &src->data, sizeof(src->data));

    return PRTE_SUCCESS;
}

int prte_dt_copy_sig(prte_grpcomm_signature_t **dest, prte_grpcomm_signature_t *src, prte_data_type_t type)
{
    *dest = PRTE_NEW(prte_grpcomm_signature_t);
    if (NULL == *dest) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    (*dest)->sz = src->sz;
    (*dest)->signature = (prte_process_name_t*)malloc(src->sz * sizeof(prte_process_name_t));
    if (NULL == (*dest)->signature) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        PRTE_RELEASE(*dest);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    memcpy((*dest)->signature, src->signature, src->sz * sizeof(prte_process_name_t));
    return PRTE_SUCCESS;
}
