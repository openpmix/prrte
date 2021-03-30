/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2009-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/prte_mca_base_framework.h"
#include "src/util/argv.h"
#include "src/util/output.h"

#include "src/mca/base/prte_mca_base_alias.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/ess/ess.h"
#include "src/mca/schizo/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/runtime.h"
#include "src/util/listener.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"

int prte_finalize(void)
{
    int rc, n;
    prte_job_t *jdata = NULL, *child_jdata = NULL, *next_jdata = NULL;

    PRTE_ACQUIRE_THREAD(&prte_init_lock);
    if (!prte_initialized) {
        PRTE_RELEASE_THREAD(&prte_init_lock);
        return PRTE_ERROR;
    }
    prte_initialized = false;
    PRTE_RELEASE_THREAD(&prte_init_lock);

    /* protect against multiple calls */
    if (prte_atomic_trylock(&prte_finalize_lock)) {
        return PRTE_SUCCESS;
    }

    /* flag that we are finalizing */
    prte_finalizing = true;

    /* stop listening for connections - will
     * be ignored if no listeners were registered */
    prte_stop_listening();

    /* release the cache */
    PRTE_RELEASE(prte_cache);

    /* Release the job hash table
     *
     * There is the potential for a prte_job_t object to still be in the
     * children list of another prte_job_t object, both objects stored in the
     * prte_job_data array. If this happens then an assert will be raised
     * when the first prte_job_t object is released when iterating over the
     * prte_job_data structure. Therefore, we traverse the children list of
     * every prte_job_t in the prte_job_data hash, removing all children
     * references before iterating over the prte_job_data hash table to
     * release the prte_job_t objects.
     */
    for (n = 0; n < prte_job_data->size; n++) {
        jdata = (prte_job_t *) prte_pointer_array_get_item(prte_job_data, n);
        if (NULL == jdata) {
            continue;
        }
        // Remove all children from the list
        // We do not want to destruct this list here since that occurs in the
        // prte_job_t destructor - which will happen in the next loop.
        PRTE_LIST_FOREACH_SAFE(child_jdata, next_jdata, &jdata->children, prte_job_t)
        {
            prte_list_remove_item(&jdata->children, &child_jdata->super);
        }
        PRTE_RELEASE(jdata);
    }
    PRTE_RELEASE(prte_job_data);

    {
        prte_pointer_array_t *array = prte_node_topologies;
        int i;
        if (array->number_free != array->size) {
            prte_mutex_lock(&array->lock);
            array->lowest_free = 0;
            array->number_free = array->size;
            for (i = 0; i < array->size; i++) {
                if (NULL != array->addr[i]) {
                    prte_topology_t *topo = (prte_topology_t *) array->addr[i];
                    topo->topo = NULL;
                    PRTE_RELEASE(topo);
                }
                array->addr[i] = NULL;
            }
            prte_mutex_unlock(&array->lock);
        }
    }
    PRTE_RELEASE(prte_node_topologies);

    {
        prte_pointer_array_t *array = prte_node_pool;
        int i;
        prte_node_t *node;
        if (array->number_free != array->size) {
            prte_mutex_lock(&array->lock);
            array->lowest_free = 0;
            array->number_free = array->size;
            for (i = 0; i < array->size; i++) {
                if (NULL != array->addr[i]) {
                    node = (prte_node_t *) array->addr[i];
                    if (NULL != node) {
                        if (NULL != node->daemon) {
                            PRTE_RELEASE(node->daemon);
                        }
                        PRTE_RELEASE(node);
                    }
                }
                array->addr[i] = NULL;
            }
            prte_mutex_unlock(&array->lock);
        }
    }
    PRTE_RELEASE(prte_node_pool);

    if (NULL != prte_fork_agent) {
        prte_argv_free(prte_fork_agent);
    }

    free(prte_process_info.nodename);
    prte_process_info.nodename = NULL;

    /* call the finalize function for this environment */
    if (PRTE_SUCCESS != (rc = prte_ess.finalize())) {
        return rc;
    }

    /* finalize schizo */
    prte_schizo.finalize();

    /* Close the general debug stream */
    prte_output_close(prte_debug_output);

    prte_mca_base_alias_cleanup();

    /* finalize the class/object system */
    prte_class_finalize();

    return PRTE_SUCCESS;
}
