/*
 * Copyright (c) 2017-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
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

#include "src/mca/propagate/base/base.h"

int prrte_propagate_base_select(void)
{
    int exit_status = PRRTE_SUCCESS;
    prrte_propagate_base_component_t *best_component = NULL;
    prrte_propagate_base_module_t *best_module = NULL;
    /*
     * Select the best component
     */
    if( PRRTE_SUCCESS != prrte_mca_base_select("propagate", prrte_propagate_base_framework.framework_output,
                &prrte_propagate_base_framework.framework_components,
                (prrte_mca_base_module_t **) &best_module,
                (prrte_mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected */
        exit_status = PRRTE_ERROR;
        goto cleanup;
    }
    PRRTE_OUTPUT_VERBOSE((5, prrte_propagate_base_framework.framework_output,
                "propagate selected"));

    /* Save the winner */
    prrte_propagate = *best_module;

    /* Initialize the winner */
    if (NULL != best_module) {
        if (PRRTE_SUCCESS != prrte_propagate.init()) {
            exit_status = PRRTE_ERROR;
            goto cleanup;
        }
    }

 cleanup:
    return exit_status;
}
