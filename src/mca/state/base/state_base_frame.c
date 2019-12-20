/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
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
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include "src/mca/mca.h"
#include "src/mca/base/base.h"

#include "src/class/prrte_list.h"
#include "src/util/output.h"

#include "src/mca/plm/plm_types.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/state/base/base.h"
#include "src/mca/state/base/state_private.h"

#include "src/mca/state/base/static-components.h"

/*
 * Globals
 */
prrte_state_base_module_t prrte_state = {0};
bool prrte_state_base_run_fdcheck = false;
int prrte_state_base_parent_fd = -1;

static int prrte_state_base_register(prrte_mca_base_register_flag_t flags)
{
    prrte_state_base_run_fdcheck = false;
    prrte_mca_base_var_register("prrte", "state", "base", "check_fds",
                                "Daemons should check fds for leaks after each job completes",
                                PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                PRRTE_INFO_LVL_9,
                                PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                &prrte_state_base_run_fdcheck);

    return PRRTE_SUCCESS;
}

static int prrte_state_base_close(void)
{
    /* Close selected component */
    if (NULL != prrte_state.finalize) {
        prrte_state.finalize();
    }

    return prrte_mca_base_framework_components_close(&prrte_state_base_framework, NULL);
}

/**
 *  * Function for finding and opening either all MCA components, or the one
 *   * that was specifically requested via a MCA parameter.
 *    */
static int prrte_state_base_open(prrte_mca_base_open_flag_t flags)
{
    /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_state_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, state, "PRRTE State Machine",
                                 prrte_state_base_register,
                                 prrte_state_base_open, prrte_state_base_close,
                                 prrte_state_base_static_components, 0);


static void prrte_state_construct(prrte_state_t *state)
{
    state->job_state = PRRTE_JOB_STATE_UNDEF;
    state->proc_state = PRRTE_PROC_STATE_UNDEF;
    state->cbfunc = NULL;
    state->priority = PRRTE_INFO_PRI;
}
PRRTE_CLASS_INSTANCE(prrte_state_t,
                   prrte_list_item_t,
                   prrte_state_construct,
                   NULL);

static void prrte_state_caddy_construct(prrte_state_caddy_t *caddy)
{
    memset(&caddy->ev, 0, sizeof(prrte_event_t));
    caddy->jdata = NULL;
}
static void prrte_state_caddy_destruct(prrte_state_caddy_t *caddy)
{
    prrte_event_del(&caddy->ev);
    if (NULL != caddy->jdata) {
        PRRTE_RELEASE(caddy->jdata);
    }
}
PRRTE_CLASS_INSTANCE(prrte_state_caddy_t,
                   prrte_object_t,
                   prrte_state_caddy_construct,
                   prrte_state_caddy_destruct);
