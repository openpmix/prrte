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
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <signal.h>
#include <string.h>

#include "src/class/prte_ring_buffer.h"
#include "src/hwloc/hwloc-internal.h"
#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/runtime/prte_progress_threads.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/path.h"
#include "src/util/printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/plm/plm_types.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/threads.h"
#include "src/util/name_fns.h"
#include "src/util/parse_options.h"
#include "src/util/show_help.h"

#include "src/mca/odls/base/base.h"
#include "src/mca/odls/base/odls_private.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prte_mca_base_component_t struct.
 */

#include "src/mca/odls/base/static-components.h"

/*
 * Instantiate globals
 */
prte_odls_base_module_t prte_odls = {0};

/*
 * Framework global variables
 */
prte_odls_globals_t prte_odls_globals = {0};

static prte_event_base_t **prte_event_base_ptr = NULL;

static int prte_odls_base_register(prte_mca_base_register_flag_t flags)
{
    prte_odls_globals.timeout_before_sigkill = 1;
    (void) prte_mca_base_var_register(
        "prte", "odls", "base", "sigkill_timeout",
        "Time to wait for a process to die after issuing a kill signal to it",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_odls_globals.timeout_before_sigkill);

    prte_odls_globals.max_threads = 32;
    (void) prte_mca_base_var_register("prte", "odls", "base", "max_threads",
                                      "Maximum number of threads to use for spawning local procs",
                                      PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                      &prte_odls_globals.max_threads);

    prte_odls_globals.num_threads = -1;
    (void) prte_mca_base_var_register("prte", "odls", "base", "num_threads",
                                      "Specific number of threads to use for spawning local procs",
                                      PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                      PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                      PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                      &prte_odls_globals.num_threads);

    prte_odls_globals.cutoff = 32;
    (void) prte_mca_base_var_register(
        "prte", "odls", "base", "cutoff",
        "Minimum number of local procs before using thread pool for spawn",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_odls_globals.cutoff);

    prte_odls_globals.signal_direct_children_only = false;
    (void) prte_mca_base_var_register(
        "prte", "odls", "base", "signal_direct_children_only",
        "Whether to restrict signals (e.g., SIGTERM) to direct children, or "
        "to apply them as well to any children spawned by those processes",
        PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_odls_globals.signal_direct_children_only);

    return PRTE_SUCCESS;
}

void prte_odls_base_harvest_threads(void)
{
    int i;

    PRTE_ACQUIRE_THREAD(&prte_odls_globals.lock);
    if (0 < prte_odls_globals.num_threads) {
        /* stop the progress threads */
        if (NULL != prte_odls_globals.ev_threads) {
            for (i = 0; NULL != prte_odls_globals.ev_threads[i]; i++) {
                prte_progress_thread_finalize(prte_odls_globals.ev_threads[i]);
            }
        }
        free(prte_odls_globals.ev_bases);
        prte_odls_globals.ev_bases = (prte_event_base_t **) malloc(sizeof(prte_event_base_t *));
        /* use the default event base */
        prte_odls_globals.ev_bases[0] = prte_event_base;
        prte_odls_globals.num_threads = 0;
        if (NULL != prte_odls_globals.ev_threads) {
            prte_argv_free(prte_odls_globals.ev_threads);
            prte_odls_globals.ev_threads = NULL;
        }
    }
    PRTE_RELEASE_THREAD(&prte_odls_globals.lock);
}

void prte_odls_base_start_threads(prte_job_t *jdata)
{
    int i;
    char *tmp;

    PRTE_ACQUIRE_THREAD(&prte_odls_globals.lock);
    /* only do this once */
    if (NULL != prte_odls_globals.ev_threads) {
        PRTE_RELEASE_THREAD(&prte_odls_globals.lock);
        return;
    }

    /* if we are a persistent DVM, expect to service lots
     * of clients */
    if (prte_persistent) {
        prte_odls_globals.num_threads = prte_odls_globals.max_threads;
        goto startup;
    }

    /* setup the pool of worker threads */
    prte_odls_globals.ev_threads = NULL;
    prte_odls_globals.next_base = 0;
    if (-1 == prte_odls_globals.num_threads) {
        if ((int) jdata->num_local_procs < prte_odls_globals.cutoff) {
            /* do not use any dedicated odls thread */
            prte_odls_globals.num_threads = 0;
        } else {
            /* user didn't specify anything, so default to some fraction of
             * the number of local procs, capping it at the max num threads
             * parameter value. */
            prte_odls_globals.num_threads = jdata->num_local_procs / 8;
            if (0 == prte_odls_globals.num_threads) {
                prte_odls_globals.num_threads = 1;
            } else if (prte_odls_globals.max_threads < prte_odls_globals.num_threads) {
                prte_odls_globals.num_threads = prte_odls_globals.max_threads;
            }
        }
    }
    if (0 == prte_odls_globals.num_threads) {
        if (NULL == prte_event_base_ptr) {
            prte_event_base_ptr = (prte_event_base_t **) malloc(sizeof(prte_event_base_t *));
            /* use the default event base */
            prte_event_base_ptr[0] = prte_event_base;
        }
        prte_odls_globals.ev_bases = prte_event_base_ptr;
    } else {
    startup:
        prte_odls_globals.ev_bases = (prte_event_base_t **) malloc(prte_odls_globals.num_threads
                                                                   * sizeof(prte_event_base_t *));
        for (i = 0; i < prte_odls_globals.num_threads; i++) {
            prte_asprintf(&tmp, "PRTE-ODLS-%d", i);
            prte_odls_globals.ev_bases[i] = prte_progress_thread_init(tmp);
            prte_argv_append_nosize(&prte_odls_globals.ev_threads, tmp);
            free(tmp);
        }
    }
    PRTE_RELEASE_THREAD(&prte_odls_globals.lock);
}

static int prte_odls_base_close(void)
{
    int i;
    prte_proc_t *proc;
    prte_list_item_t *item;

    /* cleanup ODLS globals */
    while (NULL != (item = prte_list_remove_first(&prte_odls_globals.xterm_ranks))) {
        PRTE_RELEASE(item);
    }
    PRTE_DESTRUCT(&prte_odls_globals.xterm_ranks);

    /* cleanup the global list of local children and job data */
    for (i = 0; i < prte_local_children->size; i++) {
        if (NULL != (proc = (prte_proc_t *) prte_pointer_array_get_item(prte_local_children, i))) {
            PRTE_RELEASE(proc);
        }
    }
    PRTE_RELEASE(prte_local_children);

    prte_odls_base_harvest_threads();

    PRTE_DESTRUCT_LOCK(&prte_odls_globals.lock);

    return prte_mca_base_framework_components_close(&prte_odls_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prte_odls_base_open(prte_mca_base_open_flag_t flags)
{
    char **ranks = NULL, *tmp;
    int rc, i, rank;
    prte_namelist_t *nm;
    bool xterm_hold;
    sigset_t unblock;

    PRTE_CONSTRUCT_LOCK(&prte_odls_globals.lock);
    prte_odls_globals.lock.active = false; // start with nobody having the thread

    /* initialize the global array of local children */
    prte_local_children = PRTE_NEW(prte_pointer_array_t);
    if (PRTE_SUCCESS
        != (rc = prte_pointer_array_init(prte_local_children, 1, PRTE_GLOBAL_ARRAY_MAX_SIZE, 1))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* initialize ODLS globals */
    PRTE_CONSTRUCT(&prte_odls_globals.xterm_ranks, prte_list_t);
    prte_odls_globals.xtermcmd = NULL;

    /* ensure that SIGCHLD is unblocked as we need to capture it */
    if (0 != sigemptyset(&unblock)) {
        return PRTE_ERROR;
    }
    if (0 != sigaddset(&unblock, SIGCHLD)) {
        return PRTE_ERROR;
    }
    if (0 != sigprocmask(SIG_UNBLOCK, &unblock, NULL)) {
        return PRTE_ERR_NOT_SUPPORTED;
    }

    /* check if the user requested that we display output in xterms */
    if (NULL != prte_xterm) {
        /* construct a list of ranks to be displayed */
        xterm_hold = false;
        prte_util_parse_range_options(prte_xterm, &ranks);
        for (i = 0; i < prte_argv_count(ranks); i++) {
            if (0 == strcmp(ranks[i], "BANG")) {
                xterm_hold = true;
                continue;
            }
            nm = PRTE_NEW(prte_namelist_t);
            rank = strtol(ranks[i], NULL, 10);
            if (-1 == rank) {
                /* wildcard */
                nm->name.rank = PMIX_RANK_WILDCARD;
            } else if (rank < 0) {
                /* error out on bozo case */
                prte_show_help("help-prte-odls-base.txt", "prte-odls-base:xterm-neg-rank", true,
                               rank);
                return PRTE_ERROR;
            } else {
                /* we can't check here if the rank is out of
                 * range as we don't yet know how many ranks
                 * will be in the job - we'll check later
                 */
                nm->name.rank = rank;
            }
            prte_list_append(&prte_odls_globals.xterm_ranks, &nm->super);
        }
        prte_argv_free(ranks);
        /* construct the xtermcmd */
        prte_odls_globals.xtermcmd = NULL;
        tmp = prte_find_absolute_path("xterm");
        if (NULL == tmp) {
            return PRTE_ERROR;
        }
        prte_argv_append_nosize(&prte_odls_globals.xtermcmd, tmp);
        free(tmp);
        prte_argv_append_nosize(&prte_odls_globals.xtermcmd, "-T");
        prte_argv_append_nosize(&prte_odls_globals.xtermcmd, "save");
        if (xterm_hold) {
            prte_argv_append_nosize(&prte_odls_globals.xtermcmd, "-hold");
        }
        prte_argv_append_nosize(&prte_odls_globals.xtermcmd, "-e");
    }

    /* Open up all available components */
    return prte_mca_base_framework_components_open(&prte_odls_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, odls, "PRTE Daemon Launch Subsystem", prte_odls_base_register,
                                prte_odls_base_open, prte_odls_base_close,
                                prte_odls_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

static void launch_local_const(prte_odls_launch_local_t *ptr)
{
    ptr->ev = prte_event_alloc();
    PMIX_LOAD_NSPACE(ptr->job, NULL);
    ptr->fork_local = NULL;
    ptr->retries = 0;
}
static void launch_local_dest(prte_odls_launch_local_t *ptr)
{
    prte_event_free(ptr->ev);
}
PRTE_CLASS_INSTANCE(prte_odls_launch_local_t, prte_object_t, launch_local_const, launch_local_dest);

static void sccon(prte_odls_spawn_caddy_t *p)
{
    memset(&p->opts, 0, sizeof(prte_iof_base_io_conf_t));
    p->cmd = NULL;
    p->wdir = NULL;
    p->argv = NULL;
    p->env = NULL;
}
static void scdes(prte_odls_spawn_caddy_t *p)
{
    if (NULL != p->cmd) {
        free(p->cmd);
    }
    if (NULL != p->wdir) {
        free(p->wdir);
    }
    if (NULL != p->argv) {
        prte_argv_free(p->argv);
    }
    if (NULL != p->env) {
        prte_argv_free(p->env);
    }
}
PRTE_CLASS_INSTANCE(prte_odls_spawn_caddy_t, prte_object_t, sccon, scdes);
