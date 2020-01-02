/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"

#include "src/include/constants.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/mca/reachable/reachable.h"
#include "src/mca/reachable/base/base.h"

/*
 * Globals
 */

int prrte_reachable_base_select(void)
{
    int ret;
    prrte_reachable_base_component_t *best_component = NULL;
    prrte_reachable_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PRRTE_SUCCESS != prrte_mca_base_select("reachable", prrte_reachable_base_framework.framework_output,
                                               &prrte_reachable_base_framework.framework_components,
                                               (prrte_mca_base_module_t **) &best_module,
                                               (prrte_mca_base_component_t **) &best_component, NULL) ) {
        /* notify caller that no available component found */
        return PRRTE_ERR_NOT_FOUND;
    }

    /* Save the winner */
    prrte_reachable = *best_module;

    /* Initialize the winner */
    ret = prrte_reachable.init();

    return ret;
}
