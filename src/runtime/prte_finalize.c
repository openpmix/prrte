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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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

#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_output.h"

#include "src/mca/base/pmix_mca_base_alias.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/ess/ess.h"
#include "src/mca/ras/base/base.h"
#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/prte_progress_threads.h"
#include "src/runtime/runtime.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"

int prte_finalize(void)
{
    int rc, n, i;
    prte_job_t *jdata = NULL, *child_jdata = NULL, *next_jdata = NULL;
    prte_app_context_t *app;
    prte_proc_t *p;
    prte_node_t *node;
    prte_topology_t *topo;
    prte_session_t *session;

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

#if PRTE_PMIX_STOP_PRGTHRD
    /* Stop the PMIx server's internal progress thread and wait here
     * until all active events have been processed */
    PMIx_Progress_thread_stop(NULL, 0);
#endif

    /* flag that we are finalizing */
    prte_finalizing = true;

    // stop ALL prte progress threads
    prte_progress_thread_pause(NULL);

    // we always must cleanup the session directory tree
    prte_job_session_dir_finalize(NULL);

    /* NOTE: everything below used to sit behind "#ifdef PRTE_PICKY_COMPILERS",
     * which never excluded anything: configure emits that symbol with
     * AC_DEFINE_UNQUOTED as 0 or 1, so it is always *defined* and #ifdef is
     * always true. The teardown has therefore always run in every build, and
     * it needs to - a persistent DVM's session directories and the PMIx
     * server both depend on it. Spelling it "#if PRTE_PICKY_COMPILERS" to
     * match the project rule would have silently deleted all of it from a
     * normal (non-picky) build, so the guard is gone instead of corrected. */

    /* release the cache */
    PMIX_RELEASE(prte_cache);
    PMIX_RELEASE(prte_held_jobs);
    PMIX_RELEASE(prte_prelaunch_held_jobs);
    PMIX_LIST_DESTRUCT(&prte_shrink_campaigns);
    PMIX_LIST_DESTRUCT(&prte_grow_campaigns);

    /* call the finalize function for this environment. Report a failure but
     * keep going: bailing out here would skip the rest of the teardown - and
     * PMIx_server_finalize with it - leaving the session directory tree and
     * the server's listener behind on the way out. */
    rc = prte_ess.finalize();
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    (void) pmix_mca_base_framework_close(&prte_ess_base_framework);

    /* Tear the sessions down, and with them the node pool.
     *
     * Order matters in three ways. A reservation holds a COUNTED reference on
     * each of its nodes, so it has to give those back while the nodes are
     * still alive - hence sessions before the pool. prte_default_session's
     * node array IS prte_node_pool (prte_init substitutes it), so releasing
     * that session performs the pool teardown itself and must therefore go
     * last. And session_des asks the ras base to release the underlying
     * allocation, which walks prte_ras_base.selected_modules, so the ras
     * framework has to still be open while this runs - which is why it is
     * closed just below this block rather than by the ess module that
     * opened it. Nothing may be inserted between the two.
     *
     * Note that release_allocation only cancels an allocation PRRTE itself
     * created dynamically and still tracks; a user's own resource-manager
     * allocation is left alone. So reaching it here is the correct cleanup
     * for a DVM that exits still holding a sub-allocation, not a hazard. */
    if (NULL != prte_sessions) {
        for (n = 0; n < prte_sessions->size; n++) {
            session = (prte_session_t *) pmix_pointer_array_get_item(prte_sessions, n);
            if (NULL == session || session == prte_default_session) {
                continue;
            }
            pmix_pointer_array_set_item(prte_sessions, n, NULL);
            PMIX_RELEASE(session);
        }
        if (NULL != prte_default_session) {
            /* this is what releases prte_node_pool and every node in it */
            PMIX_RELEASE(prte_default_session);
            prte_default_session = NULL;
            prte_node_pool = NULL;
        }
        PMIX_RELEASE(prte_sessions);
        prte_sessions = NULL;
    }

    /* Only reached if there was no default session to carry the pool away -
     * a process that failed partway through prte_init, or one that never
     * built one. */
    if (NULL != prte_node_pool) {
        for (n = 0; n < prte_node_pool->size; n++) {
            node = (prte_node_t *) pmix_pointer_array_get_item(prte_node_pool, n);
            if (NULL == node) {
                continue;
            }
            pmix_pointer_array_set_item(prte_node_pool, n, NULL);
            PMIX_RELEASE(node);
        }
        PMIX_RELEASE(prte_node_pool);
        prte_node_pool = NULL;
    }

    /* Every session has now had its chance to give its allocation back, so
     * the ras framework can go - which is what runs each selected module's
     * finalize, letting a component release whatever it is still holding
     * (ras/slurm, for one, carries its session stack, its shrink trackers
     * and its pending extend requests). Only the HNP ever opens ras -
     * ess/hnp does it, since no other role is permitted to allocate - so
     * only the HNP closes it. */
    if (PRTE_PROC_IS_MASTER) {
        (void) pmix_mca_base_framework_close(&prte_ras_base_framework);
    }

    for (n = 0; n < prte_job_data->size; n++) {
        jdata = (prte_job_t *) pmix_pointer_array_get_item(prte_job_data, n);
        if (NULL == jdata) {
            continue;
        }
        // Empty the children list before any job object is destroyed: a job
        // released while still linked into another job's children list trips
        // the list-item assert. Each entry carries the reference taken when
        // it was appended, so removing it means releasing it - the job pool
        // still holds the reference that keeps the child alive until this
        // loop reaches it.
        PMIX_LIST_FOREACH_SAFE(child_jdata, next_jdata, &jdata->children, prte_job_t)
        {
            pmix_list_remove_item(&jdata->children, &child_jdata->super);
            PMIX_RELEASE(child_jdata);
        }
        /* clean up any app contexts as they refcount the jdata object */
        for (i=0; i < jdata->apps->size; i++) {
            app = (prte_app_context_t*)pmix_pointer_array_get_item(jdata->apps, i);
            if (NULL != app) {
                pmix_pointer_array_set_item(jdata->apps, i, NULL);
                PMIX_RELEASE(app);
            }
        }
        // clean up any procs
        for (i=0; i < jdata->procs->size; i++) {
            p = (prte_proc_t*)pmix_pointer_array_get_item(jdata->procs, i);
            if (NULL != p) {
                pmix_pointer_array_set_item(jdata->procs, i, NULL);
                PMIX_RELEASE(p);
            }
        }
        pmix_pointer_array_set_item(prte_job_data, n, NULL);
        PMIX_RELEASE(jdata);
    }
    PMIX_RELEASE(prte_job_data);

    for (n = 0; n < prte_node_topologies->size; n++) {
        topo = (prte_topology_t *) pmix_pointer_array_get_item(prte_node_topologies, n);
        if (NULL == topo) {
            continue;
        }
        pmix_pointer_array_set_item(prte_node_topologies, n, NULL);
        PMIX_RELEASE(topo);
    }
    PMIX_RELEASE(prte_node_topologies);

    /* Close the general debug stream */
    pmix_output_close(prte_debug_output);

    pmix_mca_base_alias_cleanup();

    prte_proc_info_finalize();

    pmix_output_finalize();

    /* now shutdown PMIx - need to do this last as it finalizes
     * the utilities and class system we depend upon */
    PMIx_server_finalize();

    /* Release the event base prte_init opened.  This has to come after
     * PMIx_server_finalize(): that call can still drive server-module
     * upcalls, and every one of those thread-shifts its work onto this
     * base.  Nothing in the tree touches prte_event_base after
     * prte_finalize() returns - the tools go straight to exit(). */
    prte_event_base_close();

    return PRTE_SUCCESS;
}
