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
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "prrte_config.h"
#include "constants.h"

#include "src/util/output.h"
#include "src/util/argv.h"
#include "src/mca/base/prrte_mca_base_framework.h"

#include "src/mca/ess/ess.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_locks.h"
#include "src/runtime/runtime.h"
#include "src/util/listener.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"

int prrte_finalize(void)
{
    int rc;
    uint32_t key;
    prrte_job_t *jdata;

    --prrte_initialized;
    if (0 != prrte_initialized) {
        /* check for mismatched calls */
        if (0 > prrte_initialized) {
            prrte_output(0, "%s MISMATCHED CALLS TO PRRTE FINALIZE",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        }
        return PRRTE_ERROR;
    }

    /* protect against multiple calls */
    if (prrte_atomic_trylock(&prrte_finalize_lock)) {
        return PRRTE_SUCCESS;
    }

    /* flag that we are finalizing */
    prrte_finalizing = true;

    /* stop listening for connections - will
     * be ignored if no listeners were registered */
    prrte_stop_listening();

    /* release the cache */
    PRRTE_RELEASE(prrte_cache);

    /* release the job hash table */
    PRRTE_HASH_TABLE_FOREACH(key, uint32, jdata, prrte_job_data) {
        if (NULL != jdata) {
            PRRTE_RELEASE(jdata);
        }
    }
    PRRTE_RELEASE(prrte_job_data);

    if (prrte_do_not_launch) {
        exit(0);
    }

{
    prrte_pointer_array_t * array = prrte_node_topologies;
    int i;
    if( array->number_free != array->size ) {
        prrte_mutex_lock(&array->lock);
        array->lowest_free = 0;
        array->number_free = array->size;
        for(i=0; i<array->size; i++) {
            if(NULL != array->addr[i]) {
                prrte_topology_t * topo = (prrte_topology_t *)array->addr[i];
                topo->topo = NULL;
                PRRTE_RELEASE(topo);
            }
            array->addr[i] = NULL;
        }
        prrte_mutex_unlock(&array->lock);
    }
}
    PRRTE_RELEASE(prrte_node_topologies);

{
    prrte_pointer_array_t * array = prrte_node_pool;
    int i;
    prrte_node_t *node;
    if( array->number_free != array->size ) {
        prrte_mutex_lock(&array->lock);
        array->lowest_free = 0;
        array->number_free = array->size;
        for(i=0; i<array->size; i++) {
            if(NULL != array->addr[i]) {
                node= (prrte_node_t*)array->addr[i];
                if (NULL != node) {
                    if (NULL != node->daemon) {
                        PRRTE_RELEASE(node->daemon);
                    }
                    PRRTE_RELEASE(node);
                }
            }
            array->addr[i] = NULL;
        }
        prrte_mutex_unlock(&array->lock);
    }
}
    PRRTE_RELEASE(prrte_node_pool);
    /* call the finalize function for this environment */
    if (PRRTE_SUCCESS != (rc = prrte_ess.finalize())) {
        return rc;
    }

    /* finalize schizo */
    prrte_schizo.finalize();

    /* Close the general debug stream */
    prrte_output_close(prrte_debug_output);

    if (NULL != prrte_fork_agent) {
        prrte_argv_free(prrte_fork_agent);
    }

    /* finalize the class/object system */
    prrte_class_finalize();

    free (prrte_process_info.nodename);
    prrte_process_info.nodename = NULL;

    return PRRTE_SUCCESS;
}
