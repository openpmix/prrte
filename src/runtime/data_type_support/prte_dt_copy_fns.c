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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <string.h>

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/grpcomm/grpcomm.h"
#include "src/mca/rmaps/rmaps_types.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/attr.h"
#include "src/util/pmix_argv.h"

/**
 * JOB
 */
int prte_job_copy(prte_job_t **dest, prte_job_t *src)
{
    (*dest) = src;
    PMIX_RETAIN(src);

    return PRTE_SUCCESS;
}

/**
 * NODE
 */
int prte_node_copy(prte_node_t **dest, prte_node_t *src)
{
    prte_node_t *node;
    prte_attribute_t *kv, *kvnew;
    pmix_status_t rc;

    node = PMIX_NEW(prte_node_t);
    if (NULL != src->name) {
        node->name = strdup(src->name);
    }
    /* the alternate names a node answers to are part of its identity - a
     * copy that lacks them is unreachable by any of the names the allocation
     * actually used */
    if (NULL != src->rawname) {
        node->rawname = strdup(src->rawname);
    }
    if (NULL != src->aliases) {
        node->aliases = PMIx_Argv_copy(src->aliases);
    }
    node->state = src->state;
    node->slots = src->slots;
    node->slots_available = src->slots_available;
    node->slots_inuse = src->slots_inuse;
    node->slots_max = src->slots_max;
    if (NULL != src->topology) {
        /* the copy holds its own counted reference to the topology */
        PMIX_RETAIN(src->topology);
    }
    node->topology = src->topology;
    node->flags = src->flags;
    /* the session backpointer is borrowed, not retained (see
     * prte_node_destruct) - but it still has to come across, or a duplicated
     * node silently joins the default pool instead of the reservation its
     * original belongs to */
    node->session = src->session;

    /* attributes carry the launch-affecting per-node settings (username,
     * port, ...); a copy without them cannot be launched the same way */
    PMIX_LIST_FOREACH(kv, &src->attributes, prte_attribute_t)
    {
        kvnew = PMIX_NEW(prte_attribute_t);
        kvnew->key = kv->key;
        kvnew->local = kv->local;
        PMIX_VALUE_XFER_DIRECT(rc, &kvnew->data, &kv->data);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kvnew);
            PMIX_RELEASE(node);
            *dest = NULL;
            return prte_pmix_convert_status(rc);
        }
        pmix_list_append(&node->attributes, &kvnew->super);
    }

    (*dest) = node;

    return PRTE_SUCCESS;
}

/**
 * PROC
 */
int prte_proc_copy(prte_proc_t **dest, prte_proc_t *src)
{
    (*dest) = src;
    PMIX_RETAIN(src);
    return PRTE_SUCCESS;
}

/*
 * APP CONTEXT
 */
int prte_app_copy(prte_app_context_t **dest, prte_app_context_t *src)
{
    prte_attribute_t *kv, *kvnew;
    pmix_status_t rc;

    /* create the new object */
    *dest = PMIX_NEW(prte_app_context_t);
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
    (*dest)->state = src->state;
    (*dest)->first_rank = src->first_rank;
    (*dest)->flags = src->flags;
    (*dest)->argv = PMIx_Argv_copy(src->argv);
    (*dest)->env = PMIx_Argv_copy(src->env);
    if (NULL != src->cwd) {
        (*dest)->cwd = strdup(src->cwd);
    }

    /* app->attributes is a list of prte_attribute_t. It was being walked as
     * a list of prte_value_t, which has a different layout - so the value
     * copied out came from the wrong offset, and the attribute's key (which
     * prte_value_t does not have) was dropped entirely, leaving every copied
     * attribute unfindable. */
    PMIX_LIST_FOREACH(kv, &src->attributes, prte_attribute_t)
    {
        kvnew = PMIX_NEW(prte_attribute_t);
        kvnew->key = kv->key;
        kvnew->local = kv->local;
        PMIX_VALUE_XFER_DIRECT(rc, &kvnew->data, &kv->data);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            PMIX_RELEASE(kvnew);
            PMIX_RELEASE(*dest);
            *dest = NULL;
            return prte_pmix_convert_status(rc);
        }
        pmix_list_append(&(*dest)->attributes, &kvnew->super);
    }

    return PRTE_SUCCESS;
}

/*
 * JOB_MAP
 */
int prte_map_copy(struct prte_job_map_t **d, struct prte_job_map_t *s)
{
    int32_t i;
    prte_job_map_t **dest = (prte_job_map_t **) d;
    prte_job_map_t *src = (prte_job_map_t *) s;
    prte_node_t *node;

    if (NULL == src) {
        *dest = NULL;
        return PRTE_SUCCESS;
    }

    /* create the new object */
    *dest = PMIX_NEW(prte_job_map_t);
    if (NULL == *dest) {
        PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* copy data into it */
    if (NULL != src->req_mapper) {
        (*dest)->req_mapper = strdup(src->req_mapper);
    }
    if (NULL != src->last_mapper) {
        (*dest)->last_mapper = strdup(src->last_mapper);
    }
    (*dest)->mapping = src->mapping;
    (*dest)->ranking = src->ranking;
    (*dest)->binding = src->binding;
    (*dest)->rtos_set = src->rtos_set;
    (*dest)->num_new_daemons = src->num_new_daemons;
    (*dest)->daemon_vpid_start = src->daemon_vpid_start;
    (*dest)->num_nodes = src->num_nodes;

    /* Copy the node array one entry at a time.
     *
     * This used to blit the source array's bookkeeping (size, max_size,
     * block_size, ...) over the destination's and then memcpy addr[] across
     * it. The destination's addr[] is whatever prte_job_map_construct
     * allocated - PRTE_GLOBAL_ARRAY_BLOCK_SIZE entries - so any map spanning
     * more nodes than that wrote off the end of the heap block, and the
     * copied-in metadata then told every later reader the array was larger
     * than it is. Going through pmix_pointer_array_set_item lets the array
     * grow itself.
     *
     * Each node also has to be retained: prte_job_map_destruct releases
     * every node it holds, so a copy that took no reference of its own left
     * the two maps' destructors dropping one refcount too many between them.
     */
    for (i = 0; i < src->nodes->size; i++) {
        node = (prte_node_t *) pmix_pointer_array_get_item(src->nodes, i);
        if (NULL == node) {
            continue;
        }
        PMIX_RETAIN(node);
        if (0 > pmix_pointer_array_set_item((*dest)->nodes, i, node)) {
            PMIX_RELEASE(node);
            PMIX_RELEASE(*dest);
            *dest = NULL;
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
    }

    return PRTE_SUCCESS;
}
