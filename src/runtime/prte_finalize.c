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
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
 *                         reserved.
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
#include "src/util/pmix_argv.h"
#include "src/util/output.h"

#include "src/mca/base/prte_mca_base_alias.h"
#include "src/mca/base/prte_mca_base_var.h"
#include "src/mca/base/base.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/ess/ess.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/runtime.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"

int prte_finalize(void)
{
    int rc, n;
    prte_job_t *jdata = NULL, *child_jdata = NULL, *next_jdata = NULL;

    PMIX_ACQUIRE_THREAD(&prte_init_lock);
    if (!prte_initialized) {
        PMIX_RELEASE_THREAD(&prte_init_lock);
        return PRTE_ERROR;
    }
    prte_initialized = false;
    PMIX_RELEASE_THREAD(&prte_init_lock);

    /* protect against multiple calls */
    if (pmix_mutex_trylock(&prte_finalize_lock)) {
        return PRTE_SUCCESS;
    }

    /* flag that we are finalizing */
    prte_finalizing = true;

    /* release the cache */
    PMIX_RELEASE(prte_cache);

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
        jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, n);
        if (NULL == jdata) {
            continue;
        }
        // Remove all children from the list
        // We do not want to destruct this list here since that occurs in the
        // prte_job_t destructor - which will happen in the next loop.
        PMIX_LIST_FOREACH_SAFE(child_jdata, next_jdata, &jdata->children, prte_job_t)
        {
            pmix_list_remove_item(&jdata->children, &child_jdata->super);
        }
        PMIX_RELEASE(jdata);
    }
    PMIX_RELEASE(prte_job_data);

    {
        pmix_pointer_array_t *array = prte_node_topologies;
        int i;
        if (array->number_free != array->size) {
            array->lowest_free = 0;
            array->number_free = array->size;
            for (i = 0; i < array->size; i++) {
                if (NULL != array->addr[i]) {
                    prte_topology_t *topo = (prte_topology_t *) array->addr[i];
                    topo->topo = NULL;
                    PMIX_RELEASE(topo);
                }
                array->addr[i] = NULL;
            }
        }
    }
    PMIX_RELEASE(prte_node_topologies);

    {
        pmix_pointer_array_t *array = prte_node_pool;
        int i;
        prte_node_t *node;
        if (array->number_free != array->size) {
            array->lowest_free = 0;
            array->number_free = array->size;
            for (i = 0; i < array->size; i++) {
                if (NULL != array->addr[i]) {
                    node = (prte_node_t *) array->addr[i];
                    if (NULL != node) {
                        if (NULL != node->daemon) {
                            PMIX_RELEASE(node->daemon);
                        }
                        PMIX_RELEASE(node);
                    }
                }
                array->addr[i] = NULL;
            }
        }
    }
    PMIX_RELEASE(prte_node_pool);

    /* Close the general debug stream */
    prte_output_close(prte_debug_output);

    prte_mca_base_alias_cleanup();

    /* call the finalize function for this environment */
    if (PRTE_SUCCESS != (rc = prte_ess.finalize())) {
        return rc;
    }
    (void) prte_mca_base_framework_close(&prte_ess_base_framework);
    prte_proc_info_finalize();

    prte_output_finalize();
    prte_mca_base_var_finalize();
    prte_mca_base_close();

    /* now shutdown PMIx - need to do this last as it finalizes
     * the utilities and class system we depend upon */
    PMIx_server_finalize();

    return PRTE_SUCCESS;
}
