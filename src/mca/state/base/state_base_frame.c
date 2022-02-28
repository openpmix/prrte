/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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

#include "src/mca/base/base.h"
#include "src/mca/mca.h"

#include "src/class/pmix_list.h"
#include "src/util/output.h"

#include "src/mca/plm/plm_types.h"
#include "src/runtime/prte_globals.h"

#include "src/mca/state/base/base.h"
#include "src/mca/state/base/state_private.h"

#include "src/mca/state/base/static-components.h"

/*
 * Globals
 */
prte_state_base_module_t prte_state = {0};
bool prte_state_base_run_fdcheck = false;
int prte_state_base_parent_fd = -1;
bool prte_state_base_ready_msg = true;

static int prte_state_base_register(prte_mca_base_register_flag_t flags)
{
    PRTE_HIDE_UNUSED_PARAMS(flags);
    prte_state_base_run_fdcheck = false;
    prte_mca_base_var_register("prte", "state", "base", "check_fds",
                               "Daemons should check fds for leaks after each job completes",
                               PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE,
                               PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                               &prte_state_base_run_fdcheck);

    return PRTE_SUCCESS;
}

static int prte_state_base_close(void)
{
    /* Close selected component */
    if (NULL != prte_state.finalize) {
        prte_state.finalize();
    }

    return prte_mca_base_framework_components_close(&prte_state_base_framework, NULL);
}

/**
 *  * Function for finding and opening either all MCA components, or the one
 *   * that was specifically requested via a MCA parameter.
 *    */
static int prte_state_base_open(prte_mca_base_open_flag_t flags)
{
    /* Open up all available components */
    return prte_mca_base_framework_components_open(&prte_state_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, state, "PRTE State Machine", prte_state_base_register,
                                prte_state_base_open, prte_state_base_close,
                                prte_state_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

static void prte_state_construct(prte_state_t *state)
{
    state->job_state = PRTE_JOB_STATE_UNDEF;
    state->proc_state = PRTE_PROC_STATE_UNDEF;
    state->cbfunc = NULL;
    state->priority = PRTE_INFO_PRI;
}
PMIX_CLASS_INSTANCE(prte_state_t, pmix_list_item_t, prte_state_construct, NULL);

static void prte_state_caddy_construct(prte_state_caddy_t *caddy)
{
    memset(&caddy->ev, 0, sizeof(prte_event_t));
    caddy->jdata = NULL;
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
