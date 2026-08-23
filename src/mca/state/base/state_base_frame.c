/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif

#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"

#include "src/class/pmix_list.h"
#include "src/util/pmix_output.h"

#include "src/mca/plm/plm_types.h"
#include "src/runtime/prte_globals.h"

#include "src/mca/state/base/base.h"

#include "src/mca/state/base/static-components.h"

/*
 * Globals
 */
prte_state_base_t prte_state_base = {
    .parent_fd = -1,
    .ready_msg = true,
    .run_fdcheck = false,
    .recoverable = false,
    .max_restarts = 0,
    .continuous = false,
    .log_jobstate = false,
    .log_procstate = false,
    .log_path = NULL,
    .log_file = NULL,
    .log_fp = NULL
};
prte_state_base_module_t prte_state = {0};


static int prte_state_base_register(pmix_mca_base_register_flag_t flags)
{
    PRTE_HIDE_UNUSED_PARAMS(flags);
    prte_state_base.run_fdcheck = false;
    pmix_mca_base_var_register("prte", "state", "base", "check_fds",
                               "Daemons should check fds for leaks after each job completes",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                               &prte_state_base.run_fdcheck);

    prte_state_base.recoverable = false;
    pmix_mca_base_var_register("prte", "state", "base", "recoverable",
                               "Default setting for recoverable runtime option",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                               &prte_state_base.recoverable);

    prte_state_base.max_restarts = 0;
    pmix_mca_base_var_register("prte", "state", "base", "max_restarts",
                               "Set default max number of times to restart a failed process",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &prte_state_base.max_restarts);

    prte_state_base.continuous = false;
    pmix_mca_base_var_register("prte", "state", "base", "continuous",
                               "Set default policy for processes to run continuously until explicitly terminated",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                               &prte_state_base.continuous);

    prte_state_base.error_non_zero_exit = true;
    pmix_mca_base_var_register("prte", "state", "base", "error_non_zero_exit",
                               "Set default policy for marking it an error for a process to return a non-zero exit status",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                               &prte_state_base.error_non_zero_exit);

    prte_state_base.show_launch_progress = false;
    pmix_mca_base_var_register("prte", "state", "base", "show_launch_progress",
                               "Provide progress reports on DVM startup",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                               &prte_state_base.show_launch_progress);

    prte_state_base.notifyerrors = false;
    pmix_mca_base_var_register("prte", "state", "base", "notify_errors",
                               "Generate a PMIx event for reportable process errors",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                               &prte_state_base.notifyerrors);

    prte_state_base.autorestart = false;
    pmix_mca_base_var_register("prte", "state", "base", "autorestart",
                               "Automatically restart failed processes up to the max restart limit",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                               &prte_state_base.autorestart);

    /* DVM state logging - see state_base_log.c.  These parameters are the
     * only way to ask for it: it is deliberately not a prte.conf key, since
     * one written there would apply to every daemon of every DVM the cluster
     * starts, unattended. */
    prte_state_base.log_jobstate = false;
    pmix_mca_base_var_register("prte", "state", "base", "log_jobstate",
                               "Record every job state transition this process orders in the DVM state log",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                               &prte_state_base.log_jobstate);

    prte_state_base.log_procstate = false;
    pmix_mca_base_var_register("prte", "state", "base", "log_procstate",
                               "Record every process state transition this process orders in the DVM state log",
                               PMIX_MCA_BASE_VAR_TYPE_BOOL,
                               &prte_state_base.log_procstate);

    prte_state_base.log_path = NULL;
    pmix_mca_base_var_register("prte", "state", "base", "log_path",
                               "Directory the DVM state log is written into (a relative path is "
                               "created under the session directory base; default: that base)",
                               PMIX_MCA_BASE_VAR_TYPE_STRING,
                               &prte_state_base.log_path);

    return PRTE_SUCCESS;
}

static int prte_state_base_close(void)
{
    /* Close selected component */
    if (NULL != prte_state.finalize) {
        prte_state.finalize();
    }

    prte_state_base_log_close();

    return pmix_mca_base_framework_components_close(&prte_state_base_framework, NULL);
}

/**
 *  * Function for finding and opening either all MCA components, or the one
 *   * that was specifically requested via a MCA parameter.
 *    */
static int prte_state_base_open(pmix_mca_base_open_flag_t flags)
{
    /* Open the state log before any component can order a transition.  By
     * now prte_init_util has established our hostname and session directory
     * base, and our role (controller or daemon) is settled - all three name
     * the file. */
    prte_state_base_log_open();

    /* Open up all available components */
    return pmix_mca_base_framework_components_open(&prte_state_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(state, "PRTE State Machine", prte_state_base_register,
                                prte_state_base_open, prte_state_base_close,
                                prte_state_base_static_components,
                                PMIX_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

static void prte_state_construct(prte_state_t *state)
{
    state->job_state = PRTE_JOB_STATE_UNDEF;
    state->proc_state = PRTE_PROC_STATE_UNDEF;
    state->cbfunc = NULL;
}
PMIX_CLASS_INSTANCE(prte_state_t, pmix_list_item_t, prte_state_construct, NULL);

static void prte_state_caddy_construct(prte_state_caddy_t *caddy)
{
    /* PMIX_NEW does not zero the allocation, so every field has to be
     * initialized here.  The state/proc fields in particular are read by
     * handlers that were reached through the ERROR/ANY fallback - e.g. the
     * errmgr's job_errors does "jdata->state = caddy->job_state" - and an
     * activation that carries no job (PRTE_ACTIVATE_JOB_STATE(NULL, ...))
     * would otherwise hand them uninitialized heap.
     */
    memset(&caddy->ev, 0, sizeof(prte_event_t));
    caddy->jdata = NULL;
    caddy->job_state = PRTE_JOB_STATE_UNDEF;
    PMIX_LOAD_PROCID(&caddy->name, NULL, PMIX_RANK_INVALID);
    caddy->proc_state = PRTE_PROC_STATE_UNDEF;
}
static void prte_state_caddy_destruct(prte_state_caddy_t *caddy)
{
    prte_event_del(&caddy->ev);
    if (NULL != caddy->jdata) {
        PMIX_RELEASE(caddy->jdata);
    }
}
PMIX_CLASS_INSTANCE(prte_state_caddy_t, pmix_object_t, prte_state_caddy_construct,
                    prte_state_caddy_destruct);
