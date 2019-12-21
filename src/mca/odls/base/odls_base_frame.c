/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010-2011 Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2011-2017 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"

#include <string.h>
#include <signal.h>

#include "src/class/prrte_ring_buffer.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/runtime/prrte_progress_threads.h"
#include "src/util/output.h"
#include "src/util/path.h"
#include "src/util/argv.h"
#include "src/util/printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/plm/plm_types.h"
#include "src/runtime/prrte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/parse_options.h"
#include "src/util/show_help.h"
#include "src/threads/threads.h"

#include "src/mca/odls/base/odls_private.h"
#include "src/mca/odls/base/base.h"


/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct.
 */

#include "src/mca/odls/base/static-components.h"

/*
 * Instantiate globals
 */
prrte_odls_base_module_t prrte_odls = {0};

/*
 * Framework global variables
 */
prrte_odls_globals_t prrte_odls_globals = {0};

static prrte_event_base_t ** prrte_event_base_ptr = NULL;

static int prrte_odls_base_register(prrte_mca_base_register_flag_t flags)
{
    prrte_odls_globals.timeout_before_sigkill = 1;
    (void) prrte_mca_base_var_register("prrte", "odls", "base", "sigkill_timeout",
                                       "Time to wait for a process to die after issuing a kill signal to it",
                                       PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                       &prrte_odls_globals.timeout_before_sigkill);

    prrte_odls_globals.max_threads = 4;
    (void) prrte_mca_base_var_register("prrte", "odls", "base", "max_threads",
                                       "Maximum number of threads to use for spawning local procs",
                                       PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                       &prrte_odls_globals.max_threads);

    prrte_odls_globals.num_threads = -1;
    (void) prrte_mca_base_var_register("prrte", "odls", "base", "num_threads",
                                       "Specific number of threads to use for spawning local procs",
                                       PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                       &prrte_odls_globals.num_threads);

    prrte_odls_globals.cutoff = 32;
    (void) prrte_mca_base_var_register("prrte", "odls", "base", "cutoff",
                                       "Minimum number of local procs before using thread pool for spawn",
                                       PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                       &prrte_odls_globals.cutoff);

    prrte_odls_globals.signal_direct_children_only = false;
    (void) prrte_mca_base_var_register("prrte", "odls", "base", "signal_direct_children_only",
                                       "Whether to restrict signals (e.g., SIGTERM) to direct children, or "
                                       "to apply them as well to any children spawned by those processes",
                                       PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                       PRRTE_INFO_LVL_9,
                                       PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                       &prrte_odls_globals.signal_direct_children_only);

    return PRRTE_SUCCESS;
}

void prrte_odls_base_harvest_threads(void)
{
    int i;

    PRRTE_ACQUIRE_THREAD(&prrte_odls_globals.lock);
    if (0 < prrte_odls_globals.num_threads) {
        /* stop the progress threads */
        if (NULL != prrte_odls_globals.ev_threads) {
            for (i=0; NULL != prrte_odls_globals.ev_threads[i]; i++) {
                prrte_progress_thread_finalize(prrte_odls_globals.ev_threads[i]);
            }
        }
        free(prrte_odls_globals.ev_bases);
        prrte_odls_globals.ev_bases = (prrte_event_base_t**)malloc(sizeof(prrte_event_base_t*));
        /* use the default event base */
        prrte_odls_globals.ev_bases[0] = prrte_event_base;
        prrte_odls_globals.num_threads = 0;
        if (NULL != prrte_odls_globals.ev_threads) {
            prrte_argv_free(prrte_odls_globals.ev_threads);
            prrte_odls_globals.ev_threads = NULL;
        }
    }
    PRRTE_RELEASE_THREAD(&prrte_odls_globals.lock);
}

void prrte_odls_base_start_threads(prrte_job_t *jdata)
{
    int i;
    char *tmp;

    PRRTE_ACQUIRE_THREAD(&prrte_odls_globals.lock);
    /* only do this once */
    if (NULL != prrte_odls_globals.ev_threads) {
        PRRTE_RELEASE_THREAD(&prrte_odls_globals.lock);
        return;
    }

    /* setup the pool of worker threads */
    prrte_odls_globals.ev_threads = NULL;
    prrte_odls_globals.next_base = 0;
    if (-1 == prrte_odls_globals.num_threads) {
        if ((int)jdata->num_local_procs < prrte_odls_globals.cutoff) {
            /* do not use any dedicated odls thread */
            prrte_odls_globals.num_threads = 0;
        } else {
            /* user didn't specify anything, so default to some fraction of
             * the number of local procs, capping it at the max num threads
             * parameter value. */
            prrte_odls_globals.num_threads = jdata->num_local_procs / 8;
            if (0 == prrte_odls_globals.num_threads) {
                prrte_odls_globals.num_threads = 1;
            } else if (prrte_odls_globals.max_threads < prrte_odls_globals.num_threads) {
                prrte_odls_globals.num_threads = prrte_odls_globals.max_threads;
            }
        }
    }
    if (0 == prrte_odls_globals.num_threads) {
        if (NULL == prrte_event_base_ptr) {
            prrte_event_base_ptr = (prrte_event_base_t**)malloc(sizeof(prrte_event_base_t*));
            /* use the default event base */
            prrte_event_base_ptr[0] = prrte_event_base;
        }
        prrte_odls_globals.ev_bases = prrte_event_base_ptr;
    } else {
        prrte_odls_globals.ev_bases =
            (prrte_event_base_t**)malloc(prrte_odls_globals.num_threads * sizeof(prrte_event_base_t*));
        for (i=0; i < prrte_odls_globals.num_threads; i++) {
            prrte_asprintf(&tmp, "PRRTE-ODLS-%d", i);
            prrte_odls_globals.ev_bases[i] = prrte_progress_thread_init(tmp);
            prrte_argv_append_nosize(&prrte_odls_globals.ev_threads, tmp);
            free(tmp);
        }
    }
    PRRTE_RELEASE_THREAD(&prrte_odls_globals.lock);
}

static int prrte_odls_base_close(void)
{
    int i;
    prrte_proc_t *proc;
    prrte_list_item_t *item;

    /* cleanup ODLS globals */
    while (NULL != (item = prrte_list_remove_first(&prrte_odls_globals.xterm_ranks))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&prrte_odls_globals.xterm_ranks);

    /* cleanup the global list of local children and job data */
    for (i=0; i < prrte_local_children->size; i++) {
        if (NULL != (proc = (prrte_proc_t*)prrte_pointer_array_get_item(prrte_local_children, i))) {
            PRRTE_RELEASE(proc);
        }
    }
    PRRTE_RELEASE(prrte_local_children);

    prrte_odls_base_harvest_threads();

    PRRTE_DESTRUCT_LOCK(&prrte_odls_globals.lock);

    return prrte_mca_base_framework_components_close(&prrte_odls_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prrte_odls_base_open(prrte_mca_base_open_flag_t flags)
{
    char **ranks=NULL, *tmp;
    int rc, i, rank;
    prrte_namelist_t *nm;
    bool xterm_hold;
    sigset_t unblock;

    PRRTE_CONSTRUCT_LOCK(&prrte_odls_globals.lock);
    prrte_odls_globals.lock.active = false;   // start with nobody having the thread

    /* initialize the global array of local children */
    prrte_local_children = PRRTE_NEW(prrte_pointer_array_t);
    if (PRRTE_SUCCESS != (rc = prrte_pointer_array_init(prrte_local_children,
                                                      1,
                                                      PRRTE_GLOBAL_ARRAY_MAX_SIZE,
                                                      1))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    /* initialize ODLS globals */
    PRRTE_CONSTRUCT(&prrte_odls_globals.xterm_ranks, prrte_list_t);
    prrte_odls_globals.xtermcmd = NULL;

    /* ensure that SIGCHLD is unblocked as we need to capture it */
    if (0 != sigemptyset(&unblock)) {
        return PRRTE_ERROR;
    }
    if (0 != sigaddset(&unblock, SIGCHLD)) {
        return PRRTE_ERROR;
    }
    if (0 != sigprocmask(SIG_UNBLOCK, &unblock, NULL)) {
        return PRRTE_ERR_NOT_SUPPORTED;
    }

    /* check if the user requested that we display output in xterms */
    if (NULL != prrte_xterm) {
        /* construct a list of ranks to be displayed */
        xterm_hold = false;
        prrte_util_parse_range_options(prrte_xterm, &ranks);
        for (i=0; i < prrte_argv_count(ranks); i++) {
            if (0 == strcmp(ranks[i], "BANG")) {
                xterm_hold = true;
                continue;
            }
            nm = PRRTE_NEW(prrte_namelist_t);
            rank = strtol(ranks[i], NULL, 10);
            if (-1 == rank) {
                /* wildcard */
                nm->name.vpid = PRRTE_VPID_WILDCARD;
            } else if (rank < 0) {
                /* error out on bozo case */
                prrte_show_help("help-prrte-odls-base.txt",
                               "prrte-odls-base:xterm-neg-rank",
                               true, rank);
                return PRRTE_ERROR;
            } else {
                /* we can't check here if the rank is out of
                 * range as we don't yet know how many ranks
                 * will be in the job - we'll check later
                 */
                nm->name.vpid = rank;
            }
            prrte_list_append(&prrte_odls_globals.xterm_ranks, &nm->super);
        }
        prrte_argv_free(ranks);
        /* construct the xtermcmd */
        prrte_odls_globals.xtermcmd = NULL;
        tmp = prrte_find_absolute_path("xterm");
        if (NULL == tmp) {
            return PRRTE_ERROR;
        }
        prrte_argv_append_nosize(&prrte_odls_globals.xtermcmd, tmp);
        free(tmp);
        prrte_argv_append_nosize(&prrte_odls_globals.xtermcmd, "-T");
        prrte_argv_append_nosize(&prrte_odls_globals.xtermcmd, "save");
        if (xterm_hold) {
            prrte_argv_append_nosize(&prrte_odls_globals.xtermcmd, "-hold");
        }
        prrte_argv_append_nosize(&prrte_odls_globals.xtermcmd, "-e");
    }

     /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_odls_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, odls, "PRRTE Daemon Launch Subsystem",
                                 prrte_odls_base_register, prrte_odls_base_open, prrte_odls_base_close,
                                 prrte_odls_base_static_components, 0);

static void launch_local_const(prrte_odls_launch_local_t *ptr)
{
    ptr->ev = prrte_event_alloc();
    ptr->job = PRRTE_JOBID_INVALID;
    ptr->fork_local = NULL;
    ptr->retries = 0;
}
static void launch_local_dest(prrte_odls_launch_local_t *ptr)
{
    prrte_event_free(ptr->ev);
}
PRRTE_CLASS_INSTANCE(prrte_odls_launch_local_t,
                   prrte_object_t,
                   launch_local_const,
                   launch_local_dest);

static void sccon(prrte_odls_spawn_caddy_t *p)
{
    memset(&p->opts, 0, sizeof(prrte_iof_base_io_conf_t));
    p->cmd = NULL;
    p->wdir = NULL;
    p->argv = NULL;
    p->env = NULL;
}
static void scdes(prrte_odls_spawn_caddy_t *p)
{
    if (NULL != p->cmd) {
        free(p->cmd);
    }
    if (NULL != p->wdir) {
        free(p->wdir);
    }
    if (NULL != p->argv) {
        prrte_argv_free(p->argv);
    }
    if (NULL != p->env) {
        prrte_argv_free(p->env);
    }
}
PRRTE_CLASS_INSTANCE(prrte_odls_spawn_caddy_t,
                   prrte_object_t,
                   sccon, scdes);
