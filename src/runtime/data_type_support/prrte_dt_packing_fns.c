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
#include "types.h"

#include <sys/types.h>

#include "src/util/argv.h"
#include "src/dss/dss.h"
#include "src/dss/dss_internal.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/class/prrte_pointer_array.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/dss/dss.h"
#include "src/dss/dss_internal.h"
#include "src/runtime/data_type_support/prrte_dt_support.h"

/*
 * PRRTE_STD_CNTR
 */
int prrte_dt_pack_std_cntr(prrte_buffer_t *buffer, const void *src,
                            int32_t num_vals, prrte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    if (PRRTE_SUCCESS != (
                         ret = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_STD_CNTR_T))) {
        PRRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * JOB
 * NOTE: We do not pack all of the job object's fields as many of them have no
 * value in sending them to another location. The only purpose in packing and
 * sending a job object is to communicate the data required to dynamically
 * spawn another job - so we only pack that limited set of required data
 */
int prrte_dt_pack_job(prrte_buffer_t *buffer, const void *src,
                     int32_t num_vals, prrte_data_type_t type)
{
    int rc;
    int32_t i, j, count, bookmark;
    prrte_job_t **jobs;
    prrte_app_context_t *app;
    prrte_proc_t *proc;
    prrte_attribute_t *kv;
    prrte_list_t *cache;
    prrte_value_t *val;

    /* array of pointers to prrte_job_t objects - need to pack the objects a set of fields at a time */
    jobs = (prrte_job_t**) src;

    for (i=0; i < num_vals; i++) {
        /* pack the jobid */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                        (void*)(&(jobs[i]->jobid)), 1, PRRTE_JOBID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* pack the flags */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                        (void*)(&(jobs[i]->flags)), 1, PRRTE_JOB_FLAGS_T))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the attributes that need to be sent */
        count = 0;
        PRRTE_LIST_FOREACH(kv, &jobs[i]->attributes, prrte_attribute_t) {
            if (PRRTE_ATTR_GLOBAL == kv->local) {
                ++count;
            }
        }
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&count), 1, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        PRRTE_LIST_FOREACH(kv, &jobs[i]->attributes, prrte_attribute_t) {
            if (PRRTE_ATTR_GLOBAL == kv->local) {
                if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)&kv, 1, PRRTE_ATTRIBUTE))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
        /* check for job info attribute */
        cache = NULL;
        if (prrte_get_attribute(&jobs[i]->attributes, PRRTE_JOB_INFO_CACHE, (void**)&cache, PRRTE_PTR) &&
            NULL != cache) {
            /* we need to pack these as well, but they are composed
             * of prrte_value_t's on a list. So first pack the number
             * of list elements */
            count = prrte_list_get_size(cache);
            if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&count), 1, PRRTE_STD_CNTR))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
            /* now pack each element on the list */
            PRRTE_LIST_FOREACH(val, cache, prrte_value_t) {
                if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)&val, 1, PRRTE_VALUE))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        } else {
            /* pack a zero to indicate no job info is being passed */
            count = 0;
            if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&count), 1, PRRTE_STD_CNTR))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* pack the personality */
        count = prrte_argv_count(jobs[i]->personality);
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &count, 1, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        for (j=0; j < count; j++) {
            if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &jobs[i]->personality[j], 1, PRRTE_STRING))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* pack the number of apps */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->num_apps)), 1, PRRTE_APP_IDX))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are apps, pack the app_contexts */
        if (0 < jobs[i]->num_apps) {
            for (j=0; j < jobs[i]->apps->size; j++) {
                if (NULL == (app = (prrte_app_context_t*)prrte_pointer_array_get_item(jobs[i]->apps, j))) {
                    continue;
                }
                if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)&app, 1, PRRTE_APP_CONTEXT))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }

        /* pack the number of procs and offset */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->num_procs)), 1, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->offset)), 1, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        if (0 < jobs[i]->num_procs) {
            /* check attributes to see if this job is to be fully
             * described in the launch msg */
            if (prrte_get_attribute(&jobs[i]->attributes, PRRTE_JOB_FULLY_DESCRIBED, NULL, PRRTE_BOOL)) {
                for (j=0; j < jobs[i]->procs->size; j++) {
                    if (NULL == (proc = (prrte_proc_t*)prrte_pointer_array_get_item(jobs[i]->procs, j))) {
                        continue;
                    }
                    if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)&proc, 1, PRRTE_PROC))) {
                        PRRTE_ERROR_LOG(rc);
                        return rc;
                    }
                }
            }
        }

        /* pack the stdin target */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->stdin_target)), 1, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the total slots allocated to the job */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->total_slots_alloc)), 1, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if the map is NULL, then we cannot pack it as there is
         * nothing to pack. However, we have to flag whether or not
         * the map is included so the unpacking routine can know
         * what to do
         */
        if (NULL == jobs[i]->map) {
            /* pack a zero value */
            j=0;
        } else {
            /* pack a one to indicate a map is there */
            j = 1;
        }
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                            (void*)&j, 1, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the map - this will only pack the fields that control
         * HOW a job is to be mapped. We do -not- pack the mapped procs
         * or nodes as this info does not need to be transmitted
         */
        if (NULL != jobs[i]->map) {
            if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                             (void*)(&(jobs[i]->map)), 1, PRRTE_JOB_MAP))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* pack the bookmark */
        if (NULL == jobs[i]->bookmark) {
            bookmark = -1;
        } else {
            bookmark = jobs[i]->bookmark->index;
        }
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &bookmark, 1, PRRTE_INT32))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the job state */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->state)), 1, PRRTE_JOB_STATE))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the launcher ID */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                        (void*)(&(jobs[i]->launcher)), 1, PRRTE_JOBID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

    }
    return PRRTE_SUCCESS;
}

/*
 *  NODE
 */
int prrte_dt_pack_node(prrte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prrte_data_type_t type)
{
    int rc;
    int32_t i, count;
    prrte_node_t **nodes;
    uint8_t flag;
    prrte_attribute_t *kv;

    /* array of pointers to prrte_node_t objects - need to pack the objects a set of fields at a time */
    nodes = (prrte_node_t**) src;

    for (i=0; i < num_vals; i++) {
        /* do not pack the index - it is meaningless on the other end */

        /* pack the node name */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&(nodes[i]->name)), 1, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* do not pack the daemon name or launch id */

        /* pack the number of procs on the node */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&(nodes[i]->num_procs)), 1, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* do not pack the procs */

        /* pack whether we are oversubscribed or not */
        flag = PRRTE_FLAG_TEST(nodes[i], PRRTE_NODE_FLAG_OVERSUBSCRIBED);
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&flag), 1, PRRTE_UINT8))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the state */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&(nodes[i]->state)), 1, PRRTE_NODE_STATE))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack any shared attributes */
        count = 0;
        PRRTE_LIST_FOREACH(kv, &nodes[i]->attributes, prrte_attribute_t) {
            if (PRRTE_ATTR_GLOBAL == kv->local) {
                ++count;
            }
        }
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&count), 1, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        PRRTE_LIST_FOREACH(kv, &nodes[i]->attributes, prrte_attribute_t) {
            if (PRRTE_ATTR_GLOBAL == kv->local) {
                if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)&kv, 1, PRRTE_ATTRIBUTE))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
    }
    return PRRTE_SUCCESS;
}

/*
 * PROC
 */
int prrte_dt_pack_proc(prrte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prrte_data_type_t type)
{
    int rc;
    int32_t i, count;
    prrte_proc_t **procs;
    prrte_attribute_t *kv;

    /* array of pointers to prrte_proc_t objects - need to pack the objects a set of fields at a time */
    procs = (prrte_proc_t**) src;

    for (i=0; i < num_vals; i++) {
        /* pack the name */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->name)), 1, PRRTE_NAME))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the daemon/node it is on */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->parent)), 1, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the local rank */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->local_rank)), 1, PRRTE_LOCAL_RANK))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the node rank */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->node_rank)), 1, PRRTE_NODE_RANK))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the state */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->state)), 1, PRRTE_PROC_STATE))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the app context index */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->app_idx)), 1, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the app rank */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->app_rank)), 1, PRRTE_UINT32))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the attributes that will go */
        count = 0;
        PRRTE_LIST_FOREACH(kv, &procs[i]->attributes, prrte_attribute_t) {
            if (PRRTE_ATTR_GLOBAL == kv->local) {
                ++count;
            }
        }
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&count), 1, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        PRRTE_LIST_FOREACH(kv, &procs[i]->attributes, prrte_attribute_t) {
            if (PRRTE_ATTR_GLOBAL == kv->local) {
                if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)&kv, 1, PRRTE_ATTRIBUTE))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * APP CONTEXT
 */
int prrte_dt_pack_app_context(prrte_buffer_t *buffer, const void *src,
                             int32_t num_vals, prrte_data_type_t type)
{
    int rc;
    int32_t i, count;
    prrte_app_context_t **app_context;
    prrte_attribute_t *kv;

    /* array of pointers to prrte_app_context objects - need to pack the objects a set of fields at a time */
    app_context = (prrte_app_context_t**) src;

    for (i=0; i < num_vals; i++) {
        /* pack the application index (for multiapp jobs) */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                                                       (void*)(&(app_context[i]->idx)), 1, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the application name */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                                                       (void*)(&(app_context[i]->app)), 1, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the number of processes */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                                                       (void*)(&(app_context[i]->num_procs)), 1, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the first rank for this app */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                                                       (void*)(&(app_context[i]->first_rank)), 1, PRRTE_VPID))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the number of entries in the argv array */
        count = prrte_argv_count(app_context[i]->argv);
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&count), 1, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are entries, pack the argv entries */
        if (0 < count) {
            if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                                                           (void*)(app_context[i]->argv), count, PRRTE_STRING))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* pack the number of entries in the enviro array */
        count = prrte_argv_count(app_context[i]->env);
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&count), 1, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are entries, pack the enviro entries */
        if (0 < count) {
            if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                                                           (void*)(app_context[i]->env), count, PRRTE_STRING))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* pack the cwd */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer,
                                                       (void*)(&(app_context[i]->cwd)), 1, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack attributes */
        count = 0;
        PRRTE_LIST_FOREACH(kv, &app_context[i]->attributes, prrte_attribute_t) {
            if (PRRTE_ATTR_GLOBAL == kv->local) {
                ++count;
            }
        }
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)(&count), 1, PRRTE_STD_CNTR))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        PRRTE_LIST_FOREACH(kv, &app_context[i]->attributes, prrte_attribute_t) {
            if (PRRTE_ATTR_GLOBAL == kv->local) {
                if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, (void*)&kv, 1, PRRTE_ATTRIBUTE))) {
                    PRRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * EXIT CODE
 */
int prrte_dt_pack_exit_code(prrte_buffer_t *buffer, const void *src,
                                 int32_t num_vals, prrte_data_type_t type)
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_EXIT_CODE_T))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * NODE STATE
 */
int prrte_dt_pack_node_state(prrte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prrte_data_type_t type)
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_NODE_STATE_T))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * PROC STATE
 */
int prrte_dt_pack_proc_state(prrte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prrte_data_type_t type)
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_PROC_STATE_T))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * JOB STATE
 */
int prrte_dt_pack_job_state(prrte_buffer_t *buffer, const void *src,
                                 int32_t num_vals, prrte_data_type_t type)
{
    int rc;

    if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_JOB_STATE_T))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * JOB_MAP
 * NOTE: There is no obvious reason to include all the node information when
 * sending a map
 */
int prrte_dt_pack_map(prrte_buffer_t *buffer, const void *src,
                             int32_t num_vals, prrte_data_type_t type)
{
    int rc;
    int32_t i;
    prrte_job_map_t **maps;

    /* array of pointers to prrte_job_map_t objects - need to pack the objects a set of fields at a time */
    maps = (prrte_job_map_t**) src;

    for (i=0; i < num_vals; i++) {
        /* pack the requested mapper */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &(maps[i]->req_mapper), 1, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* pack the last mapper */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &(maps[i]->last_mapper), 1, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* pack the policies */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &(maps[i]->mapping), 1, PRRTE_MAPPING_POLICY))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &(maps[i]->ranking), 1, PRRTE_RANKING_POLICY))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &(maps[i]->binding), 1, PRRTE_BINDING_POLICY))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* pack any ppr */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &(maps[i]->ppr), 1, PRRTE_STRING))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* pack the cpus/rank */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &(maps[i]->cpus_per_rank), 1, PRRTE_INT16))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* pack the display map flag */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &(maps[i]->display_map), 1, PRRTE_BOOL))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        /* pack the number of nodes involved in the job */
        if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, &(maps[i]->num_nodes), 1, PRRTE_UINT32))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }

    }

    return PRRTE_SUCCESS;
}

/*
 * RML TAG
 */
int prrte_dt_pack_tag(prrte_buffer_t *buffer, const void *src,
                           int32_t num_vals, prrte_data_type_t type)
{
    int rc;

    /* Turn around and pack the real type */
    if (PRRTE_SUCCESS != (rc = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_RML_TAG_T))) {
        PRRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * PRRTE_DAEMON_CMD
 */
int prrte_dt_pack_daemon_cmd(prrte_buffer_t *buffer, const void *src, int32_t num_vals,
                              prrte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_DAEMON_CMD_T))) {
        PRRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * PRRTE_IOF_TAG
 */
int prrte_dt_pack_iof_tag(prrte_buffer_t *buffer, const void *src, int32_t num_vals,
                         prrte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, src, num_vals, PRRTE_IOF_TAG_T))) {
        PRRTE_ERROR_LOG(ret);
    }

    return ret;
}


/*
 * PRRTE_ATTR
 */
int prrte_dt_pack_attr(prrte_buffer_t *buffer, const void *src, int32_t num_vals,
                      prrte_data_type_t type)
{
    prrte_attribute_t **ptr;
    int32_t i, n;
    int ret;

    ptr = (prrte_attribute_t **) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack the key and type */
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->key, 1, PRRTE_ATTR_KEY_T))) {
            return ret;
        }
        if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->type, 1, PRRTE_DATA_TYPE))) {
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
        case PRRTE_TIMEVAL:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.tv, 1, PRRTE_TIMEVAL))) {
                return ret;
            }
            break;
        case PRRTE_PTR:
            /* just ignore these values */
            break;
        case PRRTE_VPID:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.vpid, 1, PRRTE_VPID))) {
                return ret;
            }
            break;
        case PRRTE_JOBID:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.jobid, 1, PRRTE_JOBID))) {
                return ret;
            }
            break;
        case PRRTE_NAME:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.name, 1, PRRTE_NAME))) {
                return ret;
            }
            break;
        case PRRTE_ENVAR:
            if (PRRTE_SUCCESS != (ret = prrte_dss_pack_buffer(buffer, &ptr[i]->data.envar, 1, PRRTE_ENVAR))) {
                return ret;
            }
            break;

        default:
            prrte_output(0, "PACK-PRRTE-ATTR: UNSUPPORTED TYPE %d", (int)ptr[i]->type);
            return PRRTE_ERROR;
        }
    }

    return PRRTE_SUCCESS;
}

/*
 * PRRTE_SIGNATURE
 */
int prrte_dt_pack_sig(prrte_buffer_t *buffer, const void *src, int32_t num_vals,
                     prrte_data_type_t type)
{
    prrte_grpcomm_signature_t **ptr;
    int32_t i;
    int rc;

    ptr = (prrte_grpcomm_signature_t **) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack the #procs */
        if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, &ptr[i]->sz, 1, PRRTE_SIZE))) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
        if (0 < ptr[i]->sz) {
            /* pack the array */
            if (PRRTE_SUCCESS != (rc = prrte_dss.pack(buffer, ptr[i]->signature, ptr[i]->sz, PRRTE_NAME))) {
                PRRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }

    return PRRTE_SUCCESS;
}
