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
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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

#include "src/hwloc/hwloc-internal.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_path.h"
#include "src/util/pmix_printf.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ess/ess.h"
#include "src/mca/plm/plm_types.h"
#include "src/runtime/prte_globals.h"
#include "src/threads/pmix_threads.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_parse_options.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/mca/odls/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public pmix_mca_base_component_t struct.
 */

#include "src/mca/odls/base/static-components.h"

/*
 * Instantiate globals
 */
prte_odls_base_module_t prte_odls = {0};

/*
 * Framework global variables
 */
prte_odls_globals_t prte_odls_globals = {
    .output = 0,
    .xterm_ranks = PMIX_LIST_STATIC_INIT,
    .xtermcmd = NULL,
    .signal_direct_children_only = false,
    .exec_agent = NULL,
    .scatter_cpusets = true,
    .pending_slices = PMIX_LIST_STATIC_INIT
};

static int prte_odls_base_register(pmix_mca_base_register_flag_t flags)
{
    PRTE_HIDE_UNUSED_PARAMS(flags);

    prte_odls_globals.signal_direct_children_only = false;
    (void) pmix_mca_base_var_register("prte", "odls", "base", "signal_direct_children_only",
                                      "Whether to restrict signals (e.g., SIGTERM) to direct children, or "
                                      "to apply them as well to any children spawned by those processes",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &prte_odls_globals.signal_direct_children_only);

    prte_odls_globals.exec_agent = NULL;
    (void) pmix_mca_base_var_register("prte", "odls", "base", "exec_agent",
                                      "Command used to exec application processes [default: NULL]",
                                      PMIX_MCA_BASE_VAR_TYPE_STRING,
                                      &prte_odls_globals.exec_agent);

    prte_odls_globals.scatter_cpusets = true;
    (void) pmix_mca_base_var_register("prte", "odls", "base", "scatter_cpusets",
                                      "Send each daemon the bindings of the procs it will launch, "
                                      "rather than broadcasting every proc's binding to every daemon",
                                      PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                      &prte_odls_globals.scatter_cpusets);

    /* A fault-injection hook, in the same spirit as prte_daemon_fail.  The
     * daemon forks its children on a worker thread and records each child's
     * pid there, while the SIGCHLD reaper runs on the progress thread and
     * has nothing but that pid with which to attribute what it reaps.  A
     * child short-lived enough to run between the fork and the store used to
     * be reaped unattributably, and the job then never completed.  The child
     * is now held until the store is done, and stalling here is what widens
     * a window that is otherwise microseconds wide and fails in a fraction
     * of a percent of launches.
     *
     * Deliberately NOT restricted to a debug build.  A timing defect has to
     * be demonstrable in the build that ships, and an optimized build is a
     * different race from a debug one; a hook that exists only in the latter
     * cannot say anything about the former.  It costs a load and a
     * predictable branch per fork(), against a fork/exec pair, and every
     * launched proc pays the stall itself only if somebody sets it. */
    prte_odls_globals.fork_publish_delay = 0;
    (void) pmix_mca_base_var_register("prte", "odls", "base", "fork_publish_delay",
                                      "Microseconds to stall between forking a child and recording "
                                      "its pid, to exercise the launch/reap race "
                                      "[default: 0 => no delay]",
                                      PMIX_MCA_BASE_VAR_TYPE_INT,
                                      &prte_odls_globals.fork_publish_delay);

    return PRTE_SUCCESS;
}

static int prte_odls_base_close(void)
{
    int i;
    prte_proc_t *proc;
    pmix_list_item_t *item;

    /* cleanup ODLS globals */
    while (NULL != (item = pmix_list_remove_first(&prte_odls_globals.xterm_ranks))) {
        PMIX_RELEASE(item);
    }
    PMIX_DESTRUCT(&prte_odls_globals.xterm_ranks);

    /* anything still here is a launch that never completed, or a slice for
     * one - PMIX_DESTRUCT would leave the items themselves behind */
    PMIX_LIST_DESTRUCT(&prte_odls_globals.pending_slices);

    /* cleanup the global list of local children and job data */
    for (i = 0; i < prte_local_children->size; i++) {
        if (NULL != (proc = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i))) {
            PMIX_RELEASE(proc);
        }
    }
    PMIX_RELEASE(prte_local_children);

    return pmix_mca_base_framework_components_close(&prte_odls_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components, or the one
 * that was specifically requested via a MCA parameter.
 */
static int prte_odls_base_open(pmix_mca_base_open_flag_t flags)
{
    char **ranks = NULL, *tmp;
    int rc, i, rank;
    prte_namelist_t *nm;
    bool xterm_hold;
    sigset_t unblock;

    /* initialize the global array of local children */
    prte_local_children = PMIX_NEW(pmix_pointer_array_t);
    if (PRTE_SUCCESS
        != (rc = pmix_pointer_array_init(prte_local_children, 1, PRTE_GLOBAL_ARRAY_MAX_SIZE, 1))) {
        PRTE_ERROR_LOG(rc);
        return rc;
    }

    /* initialize ODLS globals */
    PMIX_CONSTRUCT(&prte_odls_globals.xterm_ranks, pmix_list_t);
    PMIX_CONSTRUCT(&prte_odls_globals.pending_slices, pmix_list_t);
    prte_odls_globals.xtermcmd = NULL;

    /* ensure that SIGCHLD is unblocked as we need to capture it */
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGCHLD);

    if (0 != sigprocmask(SIG_UNBLOCK, &unblock, NULL)) {
        return PRTE_ERR_NOT_SUPPORTED;
    }

    /* check if the user requested that we display output in xterms */
    if (NULL != prte_xterm) {
        /* construct a list of ranks to be displayed */
        xterm_hold = false;
        pmix_util_parse_range_options(prte_xterm, &ranks);
        for (i = 0; i < PMIx_Argv_count(ranks); i++) {
            if (0 == strcmp(ranks[i], "BANG")) {
                xterm_hold = true;
                continue;
            }
            nm = PMIX_NEW(prte_namelist_t);
            rank = strtol(ranks[i], NULL, 10);
            if (-1 == rank) {
                /* wildcard */
                nm->name.rank = PMIX_RANK_WILDCARD;
            } else if (rank < 0) {
                /* error out on bozo case */
                prte_show_help("help-prte-odls-base.txt", "prte-odls-base:xterm-neg-rank", true,
                               rank);
                /* nm was never put on the list, and we still own the parsed
                 * range - do not walk out holding either */
                PMIX_RELEASE(nm);
                PMIx_Argv_free(ranks);
                return PRTE_ERROR;
            } else {
                /* we can't check here if the rank is out of
                 * range as we don't yet know how many ranks
                 * will be in the job - we'll check later
                 */
                nm->name.rank = rank;
            }
            pmix_list_append(&prte_odls_globals.xterm_ranks, &nm->super);
        }
        PMIx_Argv_free(ranks);
        /* construct the xtermcmd */
        prte_odls_globals.xtermcmd = NULL;
        tmp = pmix_find_absolute_path("xterm");
        if (NULL == tmp) {
            prte_show_help("help-prte-odls-base.txt", "prte-odls-base:xterm-not-found", true,
                           prte_process_info.nodename);
            return PRTE_ERROR;
        }
        PMIx_Argv_append_nosize(&prte_odls_globals.xtermcmd, tmp);
        free(tmp);
        PMIx_Argv_append_nosize(&prte_odls_globals.xtermcmd, "-T");
        PMIx_Argv_append_nosize(&prte_odls_globals.xtermcmd, "save");
        if (xterm_hold) {
            PMIx_Argv_append_nosize(&prte_odls_globals.xtermcmd, "-hold");
        }
        PMIx_Argv_append_nosize(&prte_odls_globals.xtermcmd, "-e");
    }

    /* Open up all available components */
    return pmix_mca_base_framework_components_open(&prte_odls_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(odls, "PRTE Daemon Launch Subsystem", prte_odls_base_register,
                                prte_odls_base_open, prte_odls_base_close,
                                prte_odls_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

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
PMIX_CLASS_INSTANCE(prte_odls_launch_local_t,
                    pmix_object_t,
                    launch_local_const,
                    launch_local_dest);

static void sccon(prte_odls_spawn_caddy_t *p)
{
    memset(&p->opts, 0, sizeof(prte_iof_base_io_conf_t));
    p->cmd = NULL;
    p->wdir = NULL;
    p->argv = NULL;
    p->env = NULL;
    p->bind_cpuset = NULL;
    p->bind_fatal = false;
    p->do_membind = false;
    p->membind_prep_errno = 0;
    p->membind_mode = 0;
    p->membind_nodemask = NULL;
    p->membind_maxnode = 0;
#if PRTE_HAVE_SCHED_SETAFFINITY
    p->bind_mask = NULL;
    p->bind_masksize = 0;
#endif
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
        PMIx_Argv_free(p->argv);
    }
    if (NULL != p->env) {
        PMIx_Argv_free(p->env);
    }
    if (NULL != p->bind_cpuset) {
        hwloc_bitmap_free(p->bind_cpuset);
    }
    if (NULL != p->membind_nodemask) {
        free(p->membind_nodemask);
    }
#if PRTE_HAVE_SCHED_SETAFFINITY
    if (NULL != p->bind_mask) {
        CPU_FREE(p->bind_mask);
    }
#endif
}
PMIX_CLASS_INSTANCE(prte_odls_spawn_caddy_t,
                    pmix_object_t,
                    sccon, scdes);
