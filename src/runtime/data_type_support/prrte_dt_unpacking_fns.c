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
 * Copyright (c) 2011-2017 Cisco Systems, Inc.  All rights reserved
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
#include "types.h"

#include <sys/types.h>

#include "src/dss/dss.h"
#include "src/dss/dss_internal.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/util/argv.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

/*
 * PRRTE_STD_CNTR
 */
int prrte_dt_unpack_std_cntr(prrte_buffer_t *buffer, void *dest,
                             int32_t *num_vals, prrte_data_type_t type)
{
    int ret;

    /* Turn around and unpack the real type */
    if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_STD_CNTR_T))) {
        PRRTE_ERROR_LOG(ret);
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
int prrte_dt_unpack_job(prrte_buffer_t *buffer, void *dest,
                       int32_t *num_vals, prrte_data_type_t type)
{
    int rc;
    int32_t i, k, n, count, bookmark;
    prrte_job_t **jobs;
    prrte_app_idx_t j;
    prrte_attribute_t *kv;
    char *tmp;
    prrte_value_t *val;
    prrte_list_t *cache;

    /* unpack into array of prrte_job_t objects */
    jobs = (prrte_job_t**) dest;
    for (i=0; i < *num_vals; i++) {

        /* create the prrte_job_t object */
        jobs[i] = PRRTE_NEW(prrte_job_t);
        if (NULL == jobs[i]) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        /* unpack the jobid */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                &(jobs[i]->jobid), &n, PRRTE_JOBID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* unpack the flags */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         (&(jobs[i]->flags)), &n, PRRTE_JOB_FLAGS_T))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the attributes */
        n=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &count,
                                                         &n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        for (k=0; k < count; k++) {
            n=1;
            if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &kv,
                                                             &n, PRRTE_ATTRIBUTE))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
            kv->local = PRRTE_ATTR_GLOBAL;  // obviously not a local value
            prrte_list_append(&jobs[i]->attributes, &kv->super);
        }
        /* unpack any job info */
        n=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &count,
                                                         &n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        if (0 < count){
            cache = PRRTE_NEW(prrte_list_t);
            prrte_set_attribute(&jobs[i]->attributes, PRRTE_JOB_INFO_CACHE, PRRTE_ATTR_LOCAL, (void*)cache, PRRTE_PTR);
            for (k=0; k < count; k++) {
                n=1;
                if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &val,
                                                                 &n, PRRTE_VALUE))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
                prrte_list_append(cache, &val->super);
            }
        }

        /* unpack the personality */
        n=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &count, &n, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        for (k=0; k < count; k++) {
            n=1;
            if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &tmp, &n, PRRTE_STRING))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
            prrte_argv_append_nosize(&jobs[i]->personality, tmp);
            free(tmp);
        }

        /* unpack the num apps */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                (&(jobs[i]->num_apps)), &n, PRRTE_APP_IDX))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are apps, unpack them */
        if (0 < jobs[i]->num_apps) {
            prrte_app_context_t *app;
            for (j=0; j < jobs[i]->num_apps; j++) {
                n = 1;
                if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                               &app, &n, PRRTE_APP_CONTEXT))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
                prrte_pointer_array_add(jobs[i]->apps, app);
            }
        }

        /* unpack num procs and offset */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                (&(jobs[i]->num_procs)), &n, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                (&(jobs[i]->offset)), &n, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        if (0 < jobs[i]->num_procs) {
            /* check attributes to see if this job was fully
             * described in the launch msg */
            if (prrte_get_attribute(&jobs[i]->attributes, PRRTE_JOB_FULLY_DESCRIBED, NULL, PRRTE_BOOL)) {
                prrte_proc_t *proc;
                for (j=0; j < jobs[i]->num_procs; j++) {
                    n = 1;
                    if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                   &proc, &n, PRRTE_PROC))) {
                        PRRTE_ERROR_LOG(rc);
                        return rc;
                    }
                    prrte_pointer_array_add(jobs[i]->procs, proc);
                }
            }
        }

        /* unpack stdin target */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                            (&(jobs[i]->stdin_target)), &n, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the total slots allocated to the job */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         (&(jobs[i]->total_slots_alloc)), &n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if the map is NULL, then we didn't pack it as there was
         * nothing to pack. Instead, we packed a flag to indicate whether or not
         * the map is included */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                            &j, &n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        if (0 < j) {
            /* unpack the map */
            n = 1;
            if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                            (&(jobs[i]->map)), &n, PRRTE_JOB_MAP))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* unpack the bookmark */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                            &bookmark, &n, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        if (0 <= bookmark) {
            /* retrieve it */
            jobs[i]->bookmark = (prrte_node_t*)prrte_pointer_array_get_item(prrte_node_pool, bookmark);
        }

        /* unpack the job state */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         (&(jobs[i]->state)), &n, PRRTE_JOB_STATE))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the launcher ID */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                &(jobs[i]->launcher), &n, PRRTE_JOBID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * NODE
 */
int prrte_dt_unpack_node(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type)
{
    int rc;
    int32_t i, n, k, count;
    prrte_node_t **nodes;
    uint8_t flag;
    prrte_attribute_t *kv;

    /* unpack into array of prrte_node_t objects */
    nodes = (prrte_node_t**) dest;
    for (i=0; i < *num_vals; i++) {

        /* create the node object */
        nodes[i] = PRRTE_NEW(prrte_node_t);
        if (NULL == nodes[i]) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        /* do not unpack the index - meaningless here */

        /* unpack the node name */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         &(nodes[i]->name), &n, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* do not unpack the daemon name or launch id */

        /* unpack the number of procs on the node */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         (&(nodes[i]->num_procs)), &n, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* do not unpack the proc info */

        /* unpack whether we are oversubscribed */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         (&flag), &n, PRRTE_UINT8))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        if (flag) {
            PRRTE_FLAG_SET(nodes[i], PRRTE_NODE_FLAG_OVERSUBSCRIBED);
        }

        /* unpack the state */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         (&(nodes[i]->state)), &n, PRRTE_NODE_STATE))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the attributes */
        n=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &count,
                                                         &n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        for (k=0; k < count; k++) {
            n=1;
            if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &kv,
                                                             &n, PRRTE_ATTRIBUTE))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
            kv->local = PRRTE_ATTR_GLOBAL;  // obviously not a local value
            prrte_list_append(&nodes[i]->attributes, &kv->super);
        }
    }
    return PRRTE_SUCCESS;
}

/*
 * PROC
 */
int prrte_dt_unpack_proc(prrte_buffer_t *buffer, void *dest,
                        int32_t *num_vals, prrte_data_type_t type)
{
    int rc;
    int32_t i, n, count, k;
    prrte_attribute_t *kv;;
    prrte_proc_t **procs;

    /* unpack into array of prrte_proc_t objects */
    procs = (prrte_proc_t**) dest;
    for (i=0; i < *num_vals; i++) {

        /* create the prrte_proc_t object */
        procs[i] = PRRTE_NEW(prrte_proc_t);
        if (NULL == procs[i]) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        /* unpack the name */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         &(procs[i]->name), &n, PRRTE_NAME))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the node it is on */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         (&(procs[i]->parent)), &n, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

       /* unpack the local rank */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         (&(procs[i]->local_rank)), &n, PRRTE_LOCAL_RANK))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the node rank */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                          (&(procs[i]->node_rank)), &n, PRRTE_NODE_RANK))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the state */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         (&(procs[i]->state)), &n, PRRTE_PROC_STATE))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the app context index */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         (&(procs[i]->app_idx)), &n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the app_rank */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                         (&(procs[i]->app_rank)), &n, PRRTE_UINT32))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the attributes */
        n=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &count,
                                                         &n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        for (k=0; k < count; k++) {
            n=1;
            if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &kv,
                                                             &n, PRRTE_ATTRIBUTE))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
            kv->local = PRRTE_ATTR_GLOBAL;  // obviously not a local value
            prrte_list_append(&procs[i]->attributes, &kv->super);
        }
    }
    return PRRTE_SUCCESS;
}

/*
 * APP_CONTEXT
 */
int prrte_dt_unpack_app_context(prrte_buffer_t *buffer, void *dest,
                               int32_t *num_vals, prrte_data_type_t type)
{
    int rc;
    prrte_app_context_t **app_context;
    int32_t i, max_n=1, count, k;
    prrte_attribute_t *kv;

    /* unpack into array of app_context objects */
    app_context = (prrte_app_context_t**) dest;
    for (i=0; i < *num_vals; i++) {

        /* create the app_context object */
        app_context[i] = PRRTE_NEW(prrte_app_context_t);
        if (NULL == app_context[i]) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        /* get the app index number */
        max_n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &(app_context[i]->idx),
                                                         &max_n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the application name */
        max_n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &(app_context[i]->app),
                                                         &max_n, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* get the number of processes */
        max_n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &(app_context[i]->num_procs),
                                                         &max_n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* get the first rank for this app */
        max_n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &(app_context[i]->first_rank),
                                                         &max_n, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* get the number of argv strings that were packed */
        max_n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &count, &max_n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are argv strings, allocate the required space for the char * pointers */
        if (0 < count) {
            app_context[i]->argv = (char **)malloc((count+1) * sizeof(char*));
            if (NULL == app_context[i]->argv) {
                PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                return PRRTE_ERR_OUT_OF_RESOURCE;
            }
            app_context[i]->argv[count] = NULL;

            /* and unpack them */
            max_n = count;
            if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, app_context[i]->argv, &max_n, PRRTE_STRING))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* get the number of env strings */
        max_n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &count, &max_n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are env strings, allocate the required space for the char * pointers */
        if (0 < count) {
            app_context[i]->env = (char **)malloc((count+1) * sizeof(char*));
            if (NULL == app_context[i]->env) {
                PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
                return PRRTE_ERR_OUT_OF_RESOURCE;
            }
            app_context[i]->env[count] = NULL;

            /* and unpack them */
            max_n = count;
            if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, app_context[i]->env, &max_n, PRRTE_STRING))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* unpack the cwd */
        max_n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &app_context[i]->cwd,
                                                         &max_n, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the attributes */
        max_n=1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &count,
                                                         &max_n, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        for (k=0; k < count; k++) {
            max_n=1;
            if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, &kv,
                                                             &max_n, PRRTE_ATTRIBUTE))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
            /* obviously, this isn't a local value */
            kv->local = false;
            prrte_list_append(&app_context[i]->attributes, &kv->super);
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * EXIT CODE
 */
int prrte_dt_unpack_exit_code(prrte_buffer_t *buffer, void *dest,
                                   int32_t *num_vals, prrte_data_type_t type)
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_EXIT_CODE_T))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * NODE STATE
 */
int prrte_dt_unpack_node_state(prrte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prrte_data_type_t type)
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_NODE_STATE_T))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * PROC STATE
 */
int prrte_dt_unpack_proc_state(prrte_buffer_t *buffer, void *dest,
                                    int32_t *num_vals, prrte_data_type_t type)
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_PROC_STATE_T))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * JOB STATE
 */
int prrte_dt_unpack_job_state(prrte_buffer_t *buffer, void *dest,
                                   int32_t *num_vals, prrte_data_type_t type)
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_JOB_STATE_T))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * JOB_MAP
 * NOTE: There is no obvious reason to include all the node information when
 * sending a map - hence, we do not pack that field, so don't unpack it here
 */
int prrte_dt_unpack_map(prrte_buffer_t *buffer, void *dest,
                       int32_t *num_vals, prrte_data_type_t type)
{
    int rc;
    int32_t i, n;
    prrte_job_map_t **maps;

    /* unpack into array of prrte_job_map_t objects */
    maps = (prrte_job_map_t**) dest;
    for (i=0; i < *num_vals; i++) {

        /* create the prrte_rmaps_base_map_t object */
        maps[i] = PRRTE_NEW(prrte_job_map_t);
        if (NULL == maps[i]) {
            PRRTE_ERROR_LOG(PRRTE_ERR_OUT_OF_RESOURCE);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        /* unpack the requested mapper */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->req_mapper), &n, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the last mapper */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->last_mapper), &n, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* unpack the policies */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->mapping), &n, PRRTE_MAPPING_POLICY))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->ranking), &n, PRRTE_RANKING_POLICY))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->binding), &n, PRRTE_BINDING_POLICY))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* unpack the ppr */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->ppr), &n, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* unpack the cpus/rank */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->cpus_per_rank), &n, PRRTE_INT16))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* unpack the display map flag */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->display_map), &n, PRRTE_BOOL))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* unpack the number of nodes involved in the job */
        n = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss_unpack_buffer(buffer,
                                                         &(maps[i]->num_nodes), &n, PRRTE_UINT32))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * RML_TAG
 */
int prrte_dt_unpack_tag(prrte_buffer_t *buffer, void *dest,
                       int32_t *num_vals, prrte_data_type_t type)
{
    int ret;

    /* Turn around and unpack the real type */
    if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_RML_TAG_T))) {
        PRRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * PRRTE_DAEMON_CMD
 */
int prrte_dt_unpack_daemon_cmd(prrte_buffer_t *buffer, void *dest, int32_t *num_vals,
                              prrte_data_type_t type)
{
    int ret;

    /* turn around and unpack the real type */
    ret = prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_DAEMON_CMD_T);

    return ret;
}

/*
 * PRRTE_IOF_TAG
 */
int prrte_dt_unpack_iof_tag(prrte_buffer_t *buffer, void *dest, int32_t *num_vals,
                           prrte_data_type_t type)
{
    int ret;

    /* turn around and unpack the real type */
    ret = prrte_dss_unpack_buffer(buffer, dest, num_vals, PRRTE_IOF_TAG_T);

    return ret;
}

/*
 * PRRTE_ATTR
 */

int prrte_dt_unpack_attr(prrte_buffer_t *buffer, void *dest, int32_t *num_vals,
                        prrte_data_type_t type)
{
    prrte_attribute_t **ptr;
    int32_t i, n, m;
    int ret;

    ptr = (prrte_attribute_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRRTE_NEW(prrte_attribute_t);
        if (NULL == ptr[i]) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        /* unpack the key and type */
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->key, &m, PRRTE_ATTR_KEY_T))) {
            return ret;
        }
        m=1;
        if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->type, &m, PRRTE_DATA_TYPE))) {
            return ret;
        }
        /* now unpack the right field */
        m=1;
        switch (ptr[i]->type) {
        case PRRTE_BOOL:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.flag, &m, PRRTE_BOOL))) {
                return ret;
            }
            break;
        case PRRTE_BYTE:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.byte, &m, PRRTE_BYTE))) {
                return ret;
            }
            break;
        case PRRTE_STRING:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.string, &m, PRRTE_STRING))) {
                return ret;
            }
            break;
        case PRRTE_SIZE:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.size, &m, PRRTE_SIZE))) {
                return ret;
            }
            break;
        case PRRTE_PID:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.pid, &m, PRRTE_PID))) {
                return ret;
            }
            break;
        case PRRTE_INT:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.integer, &m, PRRTE_INT))) {
                return ret;
            }
            break;
        case PRRTE_INT8:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.int8, &m, PRRTE_INT8))) {
                return ret;
            }
            break;
        case PRRTE_INT16:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.int16, &m, PRRTE_INT16))) {
                return ret;
            }
            break;
        case PRRTE_INT32:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.int32, &m, PRRTE_INT32))) {
                return ret;
            }
            break;
        case PRRTE_INT64:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.int64, &m, PRRTE_INT64))) {
                return ret;
            }
            break;
        case PRRTE_UINT:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.uint, &m, PRRTE_UINT))) {
                return ret;
            }
            break;
        case PRRTE_UINT8:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.uint8, &m, PRRTE_UINT8))) {
                return ret;
            }
            break;
        case PRRTE_UINT16:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.uint16, &m, PRRTE_UINT16))) {
                return ret;
            }
            break;
        case PRRTE_UINT32:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.uint32, &m, PRRTE_UINT32))) {
                return ret;
            }
            break;
        case PRRTE_UINT64:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.uint64, &m, PRRTE_UINT64))) {
                return ret;
            }
            break;
        case PRRTE_BYTE_OBJECT:
            /* cannot use byte object unpack as it allocates memory, so unpack object size in bytes */
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_int32(buffer, &(ptr[i]->data.bo.size), &m, PRRTE_INT32))) {
                return ret;
            }
            if (0 < ptr[i]->data.bo.size) {
                ptr[i]->data.bo.bytes = (uint8_t*)malloc(ptr[i]->data.bo.size);
                if (NULL == ptr[i]->data.bo.bytes) {
                    return PRRTE_ERR_OUT_OF_RESOURCE;
                }
                if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_byte(buffer, ptr[i]->data.bo.bytes,
                                                                &(ptr[i]->data.bo.size), PRRTE_BYTE))) {
                    return ret;
                }
            } else {
                ptr[i]->data.bo.bytes = NULL;
            }
            break;
        case PRRTE_FLOAT:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.fval, &m, PRRTE_FLOAT))) {
                return ret;
            }
            break;
        case PRRTE_TIMEVAL:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.tv, &m, PRRTE_TIMEVAL))) {
                return ret;
            }
            break;
        case PRRTE_VPID:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.vpid, &m, PRRTE_VPID))) {
                return ret;
            }
            break;
        case PRRTE_JOBID:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.jobid, &m, PRRTE_JOBID))) {
                return ret;
            }
            break;
        case PRRTE_NAME:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.name, &m, PRRTE_NAME))) {
                return ret;
            }
            break;
        case PRRTE_ENVAR:
            if (PRRTE_SUCCESS != (ret = prrte_dss_unpack_buffer(buffer, &ptr[i]->data.envar, &m, PRRTE_ENVAR))) {
                return ret;
            }
            break;

        default:
            prrte_output(0, "PACK-PRRTE-ATTR: UNSUPPORTED TYPE");
            return PRRTE_ERROR;
        }
    }

    return PRRTE_SUCCESS;
}

int prrte_dt_unpack_sig(prrte_buffer_t *buffer, void *dest, int32_t *num_vals,
                       prrte_data_type_t type)
{
    prrte_grpcomm_signature_t **ptr;
    int32_t i, n, cnt;
    int rc;

    ptr = (prrte_grpcomm_signature_t **) dest;
    n = *num_vals;

    for (i = 0; i < n; ++i) {
        /* allocate the new object */
        ptr[i] = PRRTE_NEW(prrte_grpcomm_signature_t);
        if (NULL == ptr[i]) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        /* unpack the #procs */
        cnt = 1;
        if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, &ptr[i]->sz, &cnt, PRRTE_SIZE))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        if (0 < ptr[i]->sz) {
            /* allocate space for the array */
            ptr[i]->signature = (prrte_process_name_t*)malloc(ptr[i]->sz * sizeof(prrte_process_name_t));
            /* unpack the array - the array is our signature for the collective */
            cnt = ptr[i]->sz;
            if (PRRTE_SUCCESS != (rc = prrte_dss.unpack(buffer, ptr[i]->signature, &cnt, PRRTE_NAME))) {
                PRRTE_ERROR_LOG(rc);
                PRRTE_RELEASE(ptr[i]);
                return rc;
            }
        }
    }
    return PRRTE_SUCCESS;
}
