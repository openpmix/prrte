/*
 * Copyright (c) 2017-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

#include <string.h>

#include "orte/mca/mca.h"
#include "opal/mca/base/base.h"
#include "opal/util/output.h"

#include "orte/mca/propagate/base/base.h"

int orte_propagate_base_select(void)
{
    int exit_status = OPAL_SUCCESS;
    orte_propagate_base_component_t *best_component = NULL;
    orte_propagate_base_module_t *best_module = NULL;
    /*
     * Select the best component
     */
    if( OPAL_SUCCESS != mca_base_select("propagate", orte_propagate_base_framework.framework_output,
                &orte_propagate_base_framework.framework_components,
                (mca_base_module_t **) &best_module,
                (mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected */
        exit_status = ORTE_ERROR;
        goto cleanup;
    }
    OPAL_OUTPUT_VERBOSE((5, orte_propagate_base_framework.framework_output,
                "propagate selected"));

    /* Save the winner */
    orte_propagate = *best_module;

    /* Initialize the winner */
    if (NULL != best_module) {
        if (OPAL_SUCCESS != orte_propagate.init()) {
            exit_status = OPAL_ERROR;
            goto cleanup;
        }
    }

 cleanup:
    return exit_status;
}
