/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/util/output.h"

#include "src/mca/state/base/base.h"
#include "src/mca/state/base/state_private.h"

int prrte_state_base_select(void)
{
    int exit_status = PRRTE_SUCCESS;
    prrte_state_base_component_t *best_component = NULL;
    prrte_state_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PRRTE_SUCCESS != prrte_mca_base_select("state", prrte_state_base_framework.framework_output,
                                                &prrte_state_base_framework.framework_components,
                                                (prrte_mca_base_module_t **) &best_module,
                                                (prrte_mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected */
        exit_status = PRRTE_ERROR;
        goto cleanup;
    }

    /* Save the winner */
    prrte_state = *best_module;

    /* Initialize the winner */
    if (NULL != best_module) {
        if (PRRTE_SUCCESS != prrte_state.init()) {
            exit_status = PRRTE_ERROR;
            goto cleanup;
        }
    }

 cleanup:
    return exit_status;
}
