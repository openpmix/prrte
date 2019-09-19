/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include "src/mca/mca.h"
#include "src/mca/base/base.h"


#include "src/mca/ras/base/ras_private.h"
#include "src/mca/ras/base/base.h"


/*
 * Select one RAS component from all those that are available.
 */
int prrte_ras_base_select(void)
{
    /* For all other systems, provide the following support */

    prrte_ras_base_component_t *best_component = NULL;
    prrte_ras_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PRRTE_SUCCESS != prrte_mca_base_select("ras", prrte_ras_base_framework.framework_output,
                                                &prrte_ras_base_framework.framework_components,
                                                (prrte_mca_base_module_t **) &best_module,
                                                (prrte_mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected */
        /* If we didn't find one to select, that is okay */
        return PRRTE_SUCCESS;
    }

    /* Save the winner */
    /* No component saved */
    prrte_ras_base.active_module = best_module;
    if (NULL != prrte_ras_base.active_module->init) {
        return prrte_ras_base.active_module->init();
    }

    return PRRTE_SUCCESS;
}
