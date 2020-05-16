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
#include "types.h"

#include <sys/types.h>

#include "src/dss/dss.h"
#include "src/dss/dss_internal.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/pmix/pmix-internal.h"
#include "src/util/argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/data_type_support/prte_dt_support.h"

/*
 * PRTE_STD_CNTR
 */
int prte_dt_unpack_std_cntr(prte_buffer_t *buffer, void *dest,
                             int32_t *num_vals, prte_data_type_t type)
{
    int ret;

    /* Turn around and unpack the real type */
    if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_STD_CNTR_T))) {
        PRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * JOB
 * NOTE: We do not pack all of the job object's fields as many of them have no
 * value in sending them to another location. The only purpose in packing and
 * sending a job object is to communicate the data required to dynamically
 * spawn another job - so we only pack that limited set of required data.
 * Therefore, only unpack what was packed
 */
int prte_dt_unpack_job(prte_buffer_t *buffer, void *dest,
                       int32_t *num_vals, prte_data_type_t type)
{
    int rc;
    int32_t i, k, n, count, bookmark;
    prte_job_t **jobs;
    prte_app_idx_t j;
    prte_attribute_t *kv;
    char *tmp;
    prte_value_t *val;
    prte_list_t *cache;

    /* unpack into array of prte_job_t objects */
    jobs = (prte_job_t**) dest;
    for (i=0; i < *num_vals; i++) {

        /* create the prte_job_t object */
        jobs[i] = PRTE_NEW(prte_job_t);
        if (NULL == jobs[i]) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        /* unpack the jobid */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                &(jobs[i]->jobid), &n, PRTE_JOBID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        /* unpack the nspace */
        n=1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &tmp, &n, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        PMIX_LOAD_NSPACE(jobs[i]->nspace, tmp);
        free(tmp);
        /* unpack the flags */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         (&(jobs[i]->flags)), &n, PRTE_JOB_FLAGS_T))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the attributes */
        n=1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &count,
                                                         &n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        for (k=0; k < count; k++) {
            n=1;
            if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &kv,
                                                             &n, PRTE_ATTRIBUTE))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
            kv->local = PRTE_ATTR_GLOBAL;  // obviously not a local value
            prte_list_append(&jobs[i]->attributes, &kv->super);
        }
        /* unpack any job info */
        n=1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &count,
                                                         &n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        if (0 < count){
            cache = PRTE_NEW(prte_list_t);
            prte_set_attribute(&jobs[i]->attributes, PRTE_JOB_INFO_CACHE, PRTE_ATTR_LOCAL, (void*)cache, PRTE_PTR);
            for (k=0; k < count; k++) {
                n=1;
                if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &val,
                                                                 &n, PRTE_VALUE))) {
                    PRTE_ERROR_LOG(rc);
                    return rc;
                }
                prte_list_append(cache, &val->super);
            }
        }

        /* unpack the personality */
        n=1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &count, &n, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        for (k=0; k < count; k++) {
            n=1;
            if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &tmp, &n, PRTE_STRING))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
            prte_argv_append_nosize(&jobs[i]->personality, tmp);
            free(tmp);
        }

        /* unpack the num apps */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                (&(jobs[i]->num_apps)), &n, PRTE_APP_IDX))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are apps, unpack them */
        if (0 < jobs[i]->num_apps) {
            prte_app_context_t *app;
            for (j=0; j < jobs[i]->num_apps; j++) {
                n = 1;
                if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                               &app, &n, PRTE_APP_CONTEXT))) {
                    PRTE_ERROR_LOG(rc);
                    return rc;
                }
                prte_pointer_array_add(jobs[i]->apps, app);
            }
        }

        /* unpack num procs and offset */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                (&(jobs[i]->num_procs)), &n, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                (&(jobs[i]->offset)), &n, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        if (0 < jobs[i]->num_procs) {
            /* check attributes to see if this job was fully
             * described in the launch msg */
            if (prte_get_attribute(&jobs[i]->attributes, PRTE_JOB_FULLY_DESCRIBED, NULL, PRTE_BOOL)) {
                prte_proc_t *proc;
                for (j=0; j < jobs[i]->num_procs; j++) {
                    n = 1;
                    if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                   &proc, &n, PRTE_PROC))) {
                        PRTE_ERROR_LOG(rc);
                        return rc;
                    }
                    prte_pointer_array_add(jobs[i]->procs, proc);
                }
            }
        }

        /* unpack stdin target */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                            (&(jobs[i]->stdin_target)), &n, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the total slots allocated to the job */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         (&(jobs[i]->total_slots_alloc)), &n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if the map is NULL, then we didn't pack it as there was
         * nothing to pack. Instead, we packed a flag to indicate whether or not
         * the map is included */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                            &j, &n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        if (0 < j) {
            /* unpack the map */
            n = 1;
            if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                            (&(jobs[i]->map)), &n, PRTE_JOB_MAP))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* unpack the bookmark */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                            &bookmark, &n, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        if (0 <= bookmark) {
            /* retrieve it */
            jobs[i]->bookmark = (prte_node_t*)prte_pointer_array_get_item(prte_node_pool, bookmark);
        }

        /* unpack the job state */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         (&(jobs[i]->state)), &n, PRTE_JOB_STATE))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the launcher ID */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                &(jobs[i]->launcher), &n, PRTE_JOBID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return PRTE_SUCCESS;
}

/*
 * NODE
 */
int prte_dt_unpack_node(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type)
{
    int rc;
    int32_t i, n, k, count;
    prte_node_t **nodes;
    uint8_t flag;
    prte_attribute_t *kv;

    /* unpack into array of prte_node_t objects */
    nodes = (prte_node_t**) dest;
    for (i=0; i < *num_vals; i++) {

        /* create the node object */
        nodes[i] = PRTE_NEW(prte_node_t);
        if (NULL == nodes[i]) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        /* do not unpack the index - meaningless here */

        /* unpack the node name */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         &(nodes[i]->name), &n, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* do not unpack the daemon name or launch id */

        /* unpack the number of procs on the node */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         (&(nodes[i]->num_procs)), &n, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* do not unpack the proc info */

        /* unpack whether we are oversubscribed */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         (&flag), &n, PRTE_UINT8))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        if (flag) {
            PRTE_FLAG_SET(nodes[i], PRTE_NODE_FLAG_OVERSUBSCRIBED);
        }

        /* unpack the state */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         (&(nodes[i]->state)), &n, PRTE_NODE_STATE))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the attributes */
        n=1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &count,
                                                         &n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        for (k=0; k < count; k++) {
            n=1;
            if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &kv,
                                                             &n, PRTE_ATTRIBUTE))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
            kv->local = PRTE_ATTR_GLOBAL;  // obviously not a local value
            prte_list_append(&nodes[i]->attributes, &kv->super);
        }
    }
    return PRTE_SUCCESS;
}

/*
 * PROC
 */
int prte_dt_unpack_proc(prte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prte_data_type_t type)
{
    int rc;
    int32_t i, n, count, k;
    prte_attribute_t *kv;;
    prte_proc_t **procs;

    /* unpack into array of prte_proc_t objects */
    procs = (prte_proc_t**) dest;
    for (i=0; i < *num_vals; i++) {

        /* create the prte_proc_t object */
        procs[i] = PRTE_NEW(prte_proc_t);
        if (NULL == procs[i]) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        /* unpack the name */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         &(procs[i]->name), &n, PRTE_NAME))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the node it is on */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         (&(procs[i]->parent)), &n, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

       /* unpack the local rank */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         (&(procs[i]->local_rank)), &n, PRTE_LOCAL_RANK))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the node rank */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                          (&(procs[i]->node_rank)), &n, PRTE_NODE_RANK))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the state */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         (&(procs[i]->state)), &n, PRTE_PROC_STATE))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the app context index */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         (&(procs[i]->app_idx)), &n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the app_rank */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                         (&(procs[i]->app_rank)), &n, PRTE_UINT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the attributes */
        n=1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &count,
                                                         &n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        for (k=0; k < count; k++) {
            n=1;
            if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &kv,
                                                             &n, PRTE_ATTRIBUTE))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
            kv->local = PRTE_ATTR_GLOBAL;  // obviously not a local value
            prte_list_append(&procs[i]->attributes, &kv->super);
        }
    }
    return PRTE_SUCCESS;
}

/*
 * APP_CONTEXT
 */
int prte_dt_unpack_app_context(prte_buffer_t *buffer, void *dest,
                               int32_t *num_vals, prte_data_type_t type)
{
    int rc;
    prte_app_context_t **app_context;
    int32_t i, max_n=1, count, k;
    prte_attribute_t *kv;

    /* unpack into array of app_context objects */
    app_context = (prte_app_context_t**) dest;
    for (i=0; i < *num_vals; i++) {

        /* create the app_context object */
        app_context[i] = PRTE_NEW(prte_app_context_t);
        if (NULL == app_context[i]) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        /* get the app index number */
        max_n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &(app_context[i]->idx),
                                                         &max_n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the application name */
        max_n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &(app_context[i]->app),
                                                         &max_n, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* get the number of processes */
        max_n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &(app_context[i]->num_procs),
                                                         &max_n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* get the first rank for this app */
        max_n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &(app_context[i]->first_rank),
                                                         &max_n, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* get the number of argv strings that were packed */
        max_n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &count, &max_n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are argv strings, allocate the required space for the char * pointers */
        if (0 < count) {
            app_context[i]->argv = (char **)malloc((count+1) * sizeof(char*));
            if (NULL == app_context[i]->argv) {
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            app_context[i]->argv[count] = NULL;

            /* and unpack them */
            max_n = count;
            if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, app_context[i]->argv, &max_n, PRTE_STRING))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* get the number of env strings */
        max_n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &count, &max_n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are env strings, allocate the required space for the char * pointers */
        if (0 < count) {
            app_context[i]->env = (char **)malloc((count+1) * sizeof(char*));
            if (NULL == app_context[i]->env) {
                PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
                return PRTE_ERR_OUT_OF_RESOURCE;
            }
            app_context[i]->env[count] = NULL;

            /* and unpack them */
            max_n = count;
            if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, app_context[i]->env, &max_n, PRTE_STRING))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* unpack the cwd */
        max_n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &app_context[i]->cwd,
                                                         &max_n, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the attributes */
        max_n=1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &count,
                                                         &max_n, PRTE_STD_CNTR))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        for (k=0; k < count; k++) {
            max_n=1;
            if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, &kv,
                                                             &max_n, PRTE_ATTRIBUTE))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
            /* obviously, this isn't a local value */
            kv->local = false;
            prte_list_append(&app_context[i]->attributes, &kv->super);
        }
    }

    return PRTE_SUCCESS;
}

/*
 * EXIT CODE
 */
int prte_dt_unpack_exit_code(prte_buffer_t *buffer, void *dest,
                                   int32_t *num_vals, prte_data_type_t type)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_EXIT_CODE_T))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * NODE STATE
 */
int prte_dt_unpack_node_state(prte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prte_data_type_t type)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_NODE_STATE_T))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * PROC STATE
 */
int prte_dt_unpack_proc_state(prte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prte_data_type_t type)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_PROC_STATE_T))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * JOB STATE
 */
int prte_dt_unpack_job_state(prte_buffer_t *buffer, void *dest,
                                   int32_t *num_vals, prte_data_type_t type)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_JOB_STATE_T))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * JOB_MAP
 * NOTE: There is no obvious reason to include all the node information when
 * sending a map - hence, we do not pack that field, so don't unpack it here
 */
int prte_dt_unpack_map(prte_buffer_t *buffer, void *dest,
                       int32_t *num_vals, prte_data_type_t type)
{
    int rc;
    int32_t i, n;
    prte_job_map_t **maps;

    /* unpack into array of prte_job_map_t objects */
    maps = (prte_job_map_t**) dest;
    for (i=0; i < *num_vals; i++) {

        /* create the prte_rmaps_base_map_t object */
        maps[i] = PRTE_NEW(prte_job_map_t);
        if (NULL == maps[i]) {
            PRTE_ERROR_LOG(PRTE_ERR_OUT_OF_RESOURCE);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        /* unpack the requested mapper */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->req_mapper), &n, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the last mapper */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->last_mapper), &n, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the policies */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->mapping), &n, PRTE_MAPPING_POLICY))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->ranking), &n, PRTE_RANKING_POLICY))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->binding), &n, PRTE_BINDING_POLICY))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        /* unpack the number of nodes involved in the job */
        n = 1;
        if (PRTE_SUCCESS != (rc = prte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->num_nodes), &n, PRTE_UINT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return PRTE_SUCCESS;
}

/*
 * RML_TAG
 */
int prte_dt_unpack_tag(prte_buffer_t *buffer, void *dest,
                       int32_t *num_vals, prte_data_type_t type)
{
    int ret;

    /* Turn around and unpack the real type */
    if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_RML_TAG_T))) {
        PRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * PRTE_DAEMON_CMD
 */
int prte_dt_unpack_daemon_cmd(prte_buffer_t *buffer, void *dest, int32_t *num_vals,
                              prte_data_type_t type)
{
    int ret;

    /* turn around and unpack the real type */
    ret = prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_DAEMON_CMD_T);

    return ret;
}

/*
 * PRTE_IOF_TAG
 */
int prte_dt_unpack_iof_tag(prte_buffer_t *buffer, void *dest, int32_t *num_vals,
                           prte_data_type_t type)
{
    int ret;

    /* turn around and unpack the real type */
    ret = prte_dss_unpack_buffer(buffer, dest, num_vals, PRTE_IOF_TAG_T);

    return ret;
}

/*
 * PRTE_ATTR
 */

int prte_dt_unpack_attr(prte_buffer_t *buffer, void *dest, int32_t *num_vals,
                        prte_data_type_t type)
{
    prte_attribute_t **ptr;
    int32_t i, n, m;
    int ret;

    ptr = (prte_attribute_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRTE_NEW(prte_attribute_t);
        if (NULL == ptr[i]) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        /* unpack the key and type */
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->key, &m, PRTE_ATTR_KEY_T))) {
            return ret;
        }
        m=1;
        if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->type, &m, PRTE_DATA_TYPE))) {
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
        case PRTE_TIMEVAL:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.tv, &m, PRTE_TIMEVAL))) {
                return ret;
            }
            break;
        case PRTE_VPID:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.vpid, &m, PRTE_VPID))) {
                return ret;
            }
            break;
        case PRTE_JOBID:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.jobid, &m, PRTE_JOBID))) {
                return ret;
            }
            break;
        case PRTE_NAME:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.name, &m, PRTE_NAME))) {
                return ret;
            }
            break;
        case PRTE_ENVAR:
            if (PRTE_SUCCESS != (ret = prte_dss_unpack_buffer(buffer, &ptr[i]->data.envar, &m, PRTE_ENVAR))) {
                return ret;
            }
            break;

        default:
            prte_output(0, "PACK-PRTE-ATTR: UNSUPPORTED TYPE");
            return PRTE_ERROR;
        }
    }

    return PRTE_SUCCESS;
}

int prte_dt_unpack_sig(prte_buffer_t *buffer, void *dest, int32_t *num_vals,
                       prte_data_type_t type)
{
    prte_grpcomm_signature_t **ptr;
    int32_t i, n, cnt;
    int rc;

    ptr = (prte_grpcomm_signature_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRTE_NEW(prte_grpcomm_signature_t);
        if (NULL == ptr[i]) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
        /* unpack the #procs */
        cnt = 1;
        if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, &ptr[i]->sz, &cnt, PRTE_SIZE))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        if (0 < ptr[i]->sz) {
            /* allocate space for the array */
            ptr[i]->signature = (prte_process_name_t*)malloc(ptr[i]->sz * sizeof(prte_process_name_t));
            /* unpack the array - the array is our signature for the collective */
            cnt = ptr[i]->sz;
            if (PRTE_SUCCESS != (rc = prte_dss.unpack(buffer, ptr[i]->signature, &cnt, PRTE_NAME))) {
                PRTE_ERROR_LOG(rc);
                PRTE_RELEASE(ptr[i]);
                return rc;
            }
        }
    }
    return PRTE_SUCCESS;
}
