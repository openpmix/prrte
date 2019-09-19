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
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
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

#include "src/mca/odls/base/base.h"
#include "src/mca/odls/base/odls_private.h"


/**
 * Function for selecting one component from all those that are
 * available.
 */
int prrte_odls_base_select(void)
{
    prrte_odls_base_component_t *best_component = NULL;
    prrte_odls_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PRRTE_SUCCESS != prrte_mca_base_select("odls", prrte_odls_base_framework.framework_output,
                                               &prrte_odls_base_framework.framework_components,
                                               (prrte_mca_base_module_t **) &best_module,
                                               (prrte_mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected */
        return PRRTE_ERR_NOT_FOUND;
    }

    /* Save the winner */
    prrte_odls = *best_module;

    return PRRTE_SUCCESS;
}
