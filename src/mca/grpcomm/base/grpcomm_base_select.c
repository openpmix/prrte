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
 * Copyright (c) 2013      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"

#include "src/mca/grpcomm/base/base.h"

/**
 * Function for selecting one component from all those that are
 * available.
 */
int prte_grpcomm_base_select(void)
{
    prte_grpcomm_base_component_t *best_component = NULL;
    prte_grpcomm_base_module_t *best_module = NULL;
    pmix_status_t rc;

    /*
     * Select the best component
     */
    rc = pmix_mca_base_select("grpcomm", prte_grpcomm_base_framework.framework_output,
                              &prte_grpcomm_base_framework.framework_components,
                              (pmix_mca_base_module_t **) &best_module,
                              (pmix_mca_base_component_t **) &best_component, NULL);
    if (PMIX_SUCCESS != rc) {
        /* This will only happen if no component was selected */
        return PRTE_ERR_NOT_FOUND;
    }

    /* Save the winner */
    prte_grpcomm = *best_module;
    /* give it a chance to initialize */
    if (NULL != prte_grpcomm.init) {
        prte_grpcomm.init();
    }

    return PRTE_SUCCESS;
}
