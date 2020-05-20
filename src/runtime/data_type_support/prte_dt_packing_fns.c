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

#include "src/util/argv.h"
#include "src/dss/dss.h"
#include "src/dss/dss_internal.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/class/prte_pointer_array.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/dss/dss.h"
#include "src/dss/dss_internal.h"
#include "src/runtime/data_type_support/prte_dt_support.h"

/*
 * JOB
 * NOTE: We do not pack all of the job object's fields as many of them have no
 * value in sending them to another location. The only purpose in packing and
 * sending a job object is to communicate the data required to dynamically
 * spawn another job - so we only pack that limited set of required data
 */
int prte_dt_pack_job(prte_buffer_t *buffer, const void *src,
                     int32_t num_vals, prte_data_type_t type)
{
    int rc;
    int32_t i, j, count, bookmark;
    prte_job_t **jobs;
    prte_app_context_t *app;
    prte_proc_t *proc;
    prte_attribute_t *kv;
    prte_list_t *cache;
    prte_value_t *val;
    char *tmp;

    /* array of pointers to prte_job_t objects - need to pack the objects a set of fields at a time */
    jobs = (prte_job_t**) src;

    for (i=0; i < num_vals; i++) {
        /* pack the jobid */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                        (void*)(&(jobs[i]->jobid)), 1, PRTE_JOBID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        /* pack the nspace */
        tmp = strdup(jobs[i]->nspace);
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                        (void*)(&tmp), 1, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            free(tmp);
            return rc;
        }
        free(tmp);
        /* pack the flags */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                        (void*)(&(jobs[i]->flags)), 1, PRTE_JOB_FLAGS_T))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the attributes that need to be sent */
        count = 0;
        PRTE_LIST_FOREACH(kv, &jobs[i]->attributes, prte_attribute_t) {
            if (PRTE_ATTR_GLOBAL == kv->local) {
                ++count;
            }
        }
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&count), 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        PRTE_LIST_FOREACH(kv, &jobs[i]->attributes, prte_attribute_t) {
            if (PRTE_ATTR_GLOBAL == kv->local) {
                if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)&kv, 1, PRTE_ATTRIBUTE))) {
                    PRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
        /* check for job info attribute */
        cache = NULL;
        if (prte_get_attribute(&jobs[i]->attributes, PRTE_JOB_INFO_CACHE, (void**)&cache, PRTE_PTR) &&
            NULL != cache) {
            /* we need to pack these as well, but they are composed
             * of prte_value_t's on a list. So first pack the number
             * of list elements */
            count = prte_list_get_size(cache);
            if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&count), 1, PRTE_INT32))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
            /* now pack each element on the list */
            PRTE_LIST_FOREACH(val, cache, prte_value_t) {
                if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)&val, 1, PRTE_VALUE))) {
                    PRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        } else {
            /* pack a zero to indicate no job info is being passed */
            count = 0;
            if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&count), 1, PRTE_INT32))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* pack the personality */
        count = prte_argv_count(jobs[i]->personality);
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, &count, 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        for (j=0; j < count; j++) {
            if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, &jobs[i]->personality[j], 1, PRTE_STRING))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* pack the number of apps */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->num_apps)), 1, PRTE_APP_IDX))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are apps, pack the app_contexts */
        if (0 < jobs[i]->num_apps) {
            for (j=0; j < jobs[i]->apps->size; j++) {
                if (NULL == (app = (prte_app_context_t*)prte_pointer_array_get_item(jobs[i]->apps, j))) {
                    continue;
                }
                if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)&app, 1, PRTE_APP_CONTEXT))) {
                    PRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }

        /* pack the number of procs and offset */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->num_procs)), 1, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->offset)), 1, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        if (0 < jobs[i]->num_procs) {
            /* check attributes to see if this job is to be fully
             * described in the launch msg */
            if (prte_get_attribute(&jobs[i]->attributes, PRTE_JOB_FULLY_DESCRIBED, NULL, PRTE_BOOL)) {
                for (j=0; j < jobs[i]->procs->size; j++) {
                    if (NULL == (proc = (prte_proc_t*)prte_pointer_array_get_item(jobs[i]->procs, j))) {
                        continue;
                    }
                    if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)&proc, 1, PRTE_PROC))) {
                        PRTE_ERROR_LOG(rc);
                        return rc;
                    }
                }
            }
        }

        /* pack the stdin target */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->stdin_target)), 1, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the total slots allocated to the job */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->total_slots_alloc)), 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
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
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                            (void*)&j, 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the map - this will only pack the fields that control
         * HOW a job is to be mapped. We do -not- pack the mapped procs
         * or nodes as this info does not need to be transmitted
         */
        if (NULL != jobs[i]->map) {
            if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                             (void*)(&(jobs[i]->map)), 1, PRTE_JOB_MAP))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* pack the bookmark */
        if (NULL == jobs[i]->bookmark) {
            bookmark = -1;
        } else {
            bookmark = jobs[i]->bookmark->index;
        }
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, &bookmark, 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the job state */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(jobs[i]->state)), 1, PRTE_JOB_STATE))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the launcher ID */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                        (void*)(&(jobs[i]->launcher)), 1, PRTE_JOBID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

    }
    return PRTE_SUCCESS;
}

/*
 *  NODE
 */
int prte_dt_pack_node(prte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prte_data_type_t type)
{
    int rc;
    int32_t i, count;
    prte_node_t **nodes;
    uint8_t flag;
    prte_attribute_t *kv;

    /* array of pointers to prte_node_t objects - need to pack the objects a set of fields at a time */
    nodes = (prte_node_t**) src;

    for (i=0; i < num_vals; i++) {
        /* do not pack the index - it is meaningless on the other end */

        /* pack the node name */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&(nodes[i]->name)), 1, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* do not pack the daemon name or launch id */

        /* pack the number of procs on the node */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&(nodes[i]->num_procs)), 1, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* do not pack the procs */

        /* pack whether we are oversubscribed or not */
        flag = PRTE_FLAG_TEST(nodes[i], PRTE_NODE_FLAG_OVERSUBSCRIBED);
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&flag), 1, PRTE_UINT8))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the state */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&(nodes[i]->state)), 1, PRTE_NODE_STATE))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack any shared attributes */
        count = 0;
        PRTE_LIST_FOREACH(kv, &nodes[i]->attributes, prte_attribute_t) {
            if (PRTE_ATTR_GLOBAL == kv->local) {
                ++count;
            }
        }
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&count), 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        PRTE_LIST_FOREACH(kv, &nodes[i]->attributes, prte_attribute_t) {
            if (PRTE_ATTR_GLOBAL == kv->local) {
                if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)&kv, 1, PRTE_ATTRIBUTE))) {
                    PRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
    }
    return PRTE_SUCCESS;
}

/*
 * PROC
 */
int prte_dt_pack_proc(prte_buffer_t *buffer, const void *src,
                      int32_t num_vals, prte_data_type_t type)
{
    int rc;
    int32_t i, count;
    prte_proc_t **procs;
    prte_attribute_t *kv;

    /* array of pointers to prte_proc_t objects - need to pack the objects a set of fields at a time */
    procs = (prte_proc_t**) src;

    for (i=0; i < num_vals; i++) {
        /* pack the name */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->name)), 1, PRTE_NAME))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the daemon/node it is on */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->parent)), 1, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the local rank */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->local_rank)), 1, PRTE_LOCAL_RANK))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the node rank */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->node_rank)), 1, PRTE_NODE_RANK))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the state */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->state)), 1, PRTE_PROC_STATE))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the app context index */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->app_idx)), 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the app rank */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                         (void*)(&(procs[i]->app_rank)), 1, PRTE_UINT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the attributes that will go */
        count = 0;
        PRTE_LIST_FOREACH(kv, &procs[i]->attributes, prte_attribute_t) {
            if (PRTE_ATTR_GLOBAL == kv->local) {
                ++count;
            }
        }
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&count), 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        PRTE_LIST_FOREACH(kv, &procs[i]->attributes, prte_attribute_t) {
            if (PRTE_ATTR_GLOBAL == kv->local) {
                if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)&kv, 1, PRTE_ATTRIBUTE))) {
                    PRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
    }

    return PRTE_SUCCESS;
}

/*
 * APP CONTEXT
 */
int prte_dt_pack_app_context(prte_buffer_t *buffer, const void *src,
                             int32_t num_vals, prte_data_type_t type)
{
    int rc;
    int32_t i, count;
    prte_app_context_t **app_context;
    prte_attribute_t *kv;

    /* array of pointers to prte_app_context objects - need to pack the objects a set of fields at a time */
    app_context = (prte_app_context_t**) src;

    for (i=0; i < num_vals; i++) {
        /* pack the application index (for multiapp jobs) */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                                                       (void*)(&(app_context[i]->idx)), 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the application name */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                                                       (void*)(&(app_context[i]->app)), 1, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the number of processes */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                                                       (void*)(&(app_context[i]->num_procs)), 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the first rank for this app */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                                                       (void*)(&(app_context[i]->first_rank)), 1, PRTE_VPID))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack the number of entries in the argv array */
        count = prte_argv_count(app_context[i]->argv);
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&count), 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are entries, pack the argv entries */
        if (0 < count) {
            if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                                                           (void*)(app_context[i]->argv), count, PRTE_STRING))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* pack the number of entries in the enviro array */
        count = prte_argv_count(app_context[i]->env);
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&count), 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* if there are entries, pack the enviro entries */
        if (0 < count) {
            if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                                                           (void*)(app_context[i]->env), count, PRTE_STRING))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }

        /* pack the cwd */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer,
                                                       (void*)(&(app_context[i]->cwd)), 1, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

        /* pack attributes */
        count = 0;
        PRTE_LIST_FOREACH(kv, &app_context[i]->attributes, prte_attribute_t) {
            if (PRTE_ATTR_GLOBAL == kv->local) {
                ++count;
            }
        }
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)(&count), 1, PRTE_INT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        PRTE_LIST_FOREACH(kv, &app_context[i]->attributes, prte_attribute_t) {
            if (PRTE_ATTR_GLOBAL == kv->local) {
                if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, (void*)&kv, 1, PRTE_ATTRIBUTE))) {
                    PRTE_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
    }

    return PRTE_SUCCESS;
}

/*
 * EXIT CODE
 */
int prte_dt_pack_exit_code(prte_buffer_t *buffer, const void *src,
                                 int32_t num_vals, prte_data_type_t type)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, src, num_vals, PRTE_EXIT_CODE_T))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * NODE STATE
 */
int prte_dt_pack_node_state(prte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prte_data_type_t type)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, src, num_vals, PRTE_NODE_STATE_T))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * PROC STATE
 */
int prte_dt_pack_proc_state(prte_buffer_t *buffer, const void *src,
                                  int32_t num_vals, prte_data_type_t type)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, src, num_vals, PRTE_PROC_STATE_T))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * JOB STATE
 */
int prte_dt_pack_job_state(prte_buffer_t *buffer, const void *src,
                                 int32_t num_vals, prte_data_type_t type)
{
    int rc;

    if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, src, num_vals, PRTE_JOB_STATE_T))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * JOB_MAP
 * NOTE: There is no obvious reason to include all the node information when
 * sending a map
 */
int prte_dt_pack_map(prte_buffer_t *buffer, const void *src,
                             int32_t num_vals, prte_data_type_t type)
{
    int rc;
    int32_t i;
    prte_job_map_t **maps;

    /* array of pointers to prte_job_map_t objects - need to pack the objects a set of fields at a time */
    maps = (prte_job_map_t**) src;

    for (i=0; i < num_vals; i++) {
        /* pack the requested mapper */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, &(maps[i]->req_mapper), 1, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        /* pack the last mapper */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, &(maps[i]->last_mapper), 1, PRTE_STRING))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        /* pack the policies */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, &(maps[i]->mapping), 1, PRTE_MAPPING_POLICY))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, &(maps[i]->ranking), 1, PRTE_RANKING_POLICY))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, &(maps[i]->binding), 1, PRTE_BINDING_POLICY))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        /* pack the number of nodes involved in the job */
        if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, &(maps[i]->num_nodes), 1, PRTE_UINT32))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }

    }

    return PRTE_SUCCESS;
}

/*
 * RML TAG
 */
int prte_dt_pack_tag(prte_buffer_t *buffer, const void *src,
                           int32_t num_vals, prte_data_type_t type)
{
    int rc;

    /* Turn around and pack the real type */
    if (PRTE_SUCCESS != (rc = prte_dss_pack_buffer(buffer, src, num_vals, PRTE_RML_TAG_T))) {
        PRTE_ERROR_LOG(rc);
    }

    return rc;
}

/*
 * PRTE_DAEMON_CMD
 */
int prte_dt_pack_daemon_cmd(prte_buffer_t *buffer, const void *src, int32_t num_vals,
                              prte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, src, num_vals, PRTE_DAEMON_CMD_T))) {
        PRTE_ERROR_LOG(ret);
    }

    return ret;
}

/*
 * PRTE_IOF_TAG
 */
int prte_dt_pack_iof_tag(prte_buffer_t *buffer, const void *src, int32_t num_vals,
                         prte_data_type_t type)
{
    int ret;

    /* Turn around and pack the real type */
    if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, src, num_vals, PRTE_IOF_TAG_T))) {
        PRTE_ERROR_LOG(ret);
    }

    return ret;
}


/*
 * PRTE_ATTR
 */
int prte_dt_pack_attr(prte_buffer_t *buffer, const void *src, int32_t num_vals,
                      prte_data_type_t type)
{
    prte_attribute_t **ptr;
    int32_t i, n;
    int ret;

    ptr = (prte_attribute_t **) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack the key and type */
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->key, 1, PRTE_ATTR_KEY_T))) {
            return ret;
        }
        if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->type, 1, PRTE_DATA_TYPE))) {
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
        case PRTE_TIMEVAL:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.tv, 1, PRTE_TIMEVAL))) {
                return ret;
            }
            break;
        case PRTE_PTR:
            /* just ignore these values */
            break;
        case PRTE_VPID:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.vpid, 1, PRTE_VPID))) {
                return ret;
            }
            break;
        case PRTE_JOBID:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.jobid, 1, PRTE_JOBID))) {
                return ret;
            }
            break;
        case PRTE_NAME:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.name, 1, PRTE_NAME))) {
                return ret;
            }
            break;
        case PRTE_ENVAR:
            if (PRTE_SUCCESS != (ret = prte_dss_pack_buffer(buffer, &ptr[i]->data.envar, 1, PRTE_ENVAR))) {
                return ret;
            }
            break;

        default:
            prte_output(0, "PACK-PRTE-ATTR: UNSUPPORTED TYPE %d", (int)ptr[i]->type);
            return PRTE_ERROR;
        }
    }

    return PRTE_SUCCESS;
}

/*
 * PRTE_SIGNATURE
 */
int prte_dt_pack_sig(prte_buffer_t *buffer, const void *src, int32_t num_vals,
                     prte_data_type_t type)
{
    prte_grpcomm_signature_t **ptr;
    int32_t i;
    int rc;

    ptr = (prte_grpcomm_signature_t **) src;

    for (i = 0; i < num_vals; ++i) {
        /* pack the #procs */
        if (PRTE_SUCCESS != (rc = prte_dss.pack(buffer, &ptr[i]->sz, 1, PRTE_SIZE))) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
        if (0 < ptr[i]->sz) {
            /* pack the array */
            if (PRTE_SUCCESS != (rc = prte_dss.pack(buffer, ptr[i]->signature, ptr[i]->sz, PRTE_NAME))) {
                PRTE_ERROR_LOG(rc);
                return rc;
            }
        }
    }

    return PRTE_SUCCESS;
}
