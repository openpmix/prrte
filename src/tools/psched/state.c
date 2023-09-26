/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2023 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <string.h>

#include "src/pmix/pmix-internal.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/ras/base/base.h"
#include "src/runtime/prte_quit.h"

#include "src/mca/state/base/base.h"
#include "psched.h"

/*
 * Module functions: Global
 */
static int init(void);
static int finalize(void);

/* local functions */
static void alloc_complete(int fd, short args, void *cbata);

/* defined default state machine sequence - individual
 * plm's must add a state for launching daemons
 */
static prte_job_state_t launch_states[] = {
    PRTE_JOB_STATE_ALLOCATE,
    PRTE_JOB_STATE_ALLOCATION_COMPLETE
};

static prte_state_cbfunc_t launch_callbacks[] = {
    prte_ras_base_allocate,
    alloc_complete
};

static void force_quit(int fd, short args, void *cbdata)
{
    PRTE_HIDE_UNUSED_PARAMS(fd, args);
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;

    prte_event_base_active = false;
    PMIX_RELEASE(caddy);
}

/************************
 * Local variables
 ************************/
static bool terminate_dvm = false;
static bool dvm_terminated = false;


prte_state_base_module_t psched_state_module = {
    .init = init,
    .finalize = finalize,
    .activate_job_state = prte_state_base_activate_job_state,
    .add_job_state = prte_state_base_add_job_state,
    .set_job_state_callback = prte_state_base_set_job_state_callback,
    .set_job_state_priority = prte_state_base_set_job_state_priority,
    .remove_job_state = prte_state_base_remove_job_state,
    .activate_proc_state = prte_state_base_activate_proc_state,
    .add_proc_state = prte_state_base_add_proc_state,
    .set_proc_state_callback = prte_state_base_set_proc_state_callback,
    .set_proc_state_priority = prte_state_base_set_proc_state_priority,
    .remove_proc_state = prte_state_base_remove_proc_state
};

static int state_base_verbose = -1;
void psched_state_init(void)
{
    pmix_output_stream_t lds;

    pmix_mca_base_var_register("prte", "state", "base", "verbose",
                               "Verbosity for debugging state machine",
                               PMIX_MCA_BASE_VAR_TYPE_INT,
                               &state_base_verbose);
    if (0 <= state_base_verbose) {
        PMIX_CONSTRUCT(&lds, pmix_output_stream_t);
        lds.lds_want_stdout = true;
        prte_state_base_framework.framework_output = pmix_output_open(&lds);
        PMIX_DESTRUCT(&lds);
        pmix_output_set_verbosity(prte_state_base_framework.framework_output, state_base_verbose);
    }

    prte_state = psched_state_module;
    psched_state_module.init();
}

/************************
 * API Definitions
 ************************/
static int init(void)
{
    int i, rc;
    int num_states;

    pmix_output_verbose(2, prte_state_base_framework.framework_output,
                        "%s state:psched: initialize",
                        PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));

    /* setup the state machines */
    PMIX_CONSTRUCT(&prte_job_states, pmix_list_t);
    PMIX_CONSTRUCT(&prte_proc_states, pmix_list_t);

    /* setup the job state machine */
    num_states = sizeof(launch_states) / sizeof(prte_job_state_t);
    for (i = 0; i < num_states; i++) {
        if (PRTE_SUCCESS
            != (rc = prte_state.add_job_state(launch_states[i], launch_callbacks[i],
                                              PRTE_SYS_PRI))) {
            PRTE_ERROR_LOG(rc);
        }
    }
    /* add the termination response */
    rc = prte_state.add_job_state(PRTE_JOB_STATE_DAEMONS_TERMINATED, prte_quit, PRTE_SYS_PRI);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    /* add a default error response */
    rc = prte_state.add_job_state(PRTE_JOB_STATE_FORCED_EXIT, force_quit, PRTE_ERROR_PRI);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    /* add callback to report progress, if requested */
    rc = prte_state.add_job_state(PRTE_JOB_STATE_REPORT_PROGRESS,
                                  prte_state_base_report_progress, PRTE_ERROR_PRI);
    if (PRTE_SUCCESS != rc) {
        PRTE_ERROR_LOG(rc);
    }
    if (5 < pmix_output_get_verbosity(prte_state_base_framework.framework_output)) {
        prte_state_base_print_job_state_machine();
    }

    return PRTE_SUCCESS;
}

static int finalize(void)
{
    /* cleanup the state machines */
    PMIX_LIST_DESTRUCT(&prte_proc_states);
    PMIX_LIST_DESTRUCT(&prte_job_states);

    return PRTE_SUCCESS;
}

static void alloc_complete(int fd, short args, void *cbdata)
{
    prte_state_caddy_t *caddy = (prte_state_caddy_t *) cbdata;
    PRTE_HIDE_UNUSED_PARAMS(fd, args);

    PMIX_RELEASE(caddy);
}
