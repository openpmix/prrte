/*
 * Copyright (c) 2017-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/util/output.h"

#include "src/mca/propagate/base/base.h"

int prte_propagate_base_select(void)
{
    int exit_status = PRTE_SUCCESS;
    prte_propagate_base_component_t *best_component = NULL;
    prte_propagate_base_module_t *best_module = NULL;
    /* early bailout. */
    if (!prte_enable_ft) return PRTE_SUCCESS;
    /*
     * Select the best component
     */
    if( PRTE_SUCCESS != prte_mca_base_select("propagate", prte_propagate_base_framework.framework_output,
                &prte_propagate_base_framework.framework_components,
                (prte_mca_base_module_t **) &best_module,
                (prte_mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected */
        exit_status = PRTE_ERROR;
        goto cleanup;
    }
    PRTE_OUTPUT_VERBOSE((5, prte_propagate_base_framework.framework_output,
                "propagate selected"));

    if (NULL == best_module) {
        /* nobody home */
        exit_status = PRTE_ERROR;
        goto cleanup;
    }

    /* Save the winner */
    prte_propagate = *best_module;

    /* Initialize the winner */
    if (NULL != best_module) {
        if (PRTE_SUCCESS != prte_propagate.init()) {
            exit_status = PRTE_ERROR;
            goto cleanup;
        }
    }

 cleanup:
    return exit_status;
}
