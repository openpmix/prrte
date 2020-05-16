/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prte_config.h"

#include "constants.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/mca/pstat/pstat.h"
#include "src/mca/pstat/base/base.h"

/*
 * Globals
 */

int prte_pstat_base_select(void)
{
    int ret, exit_status = PRTE_SUCCESS;
    prte_pstat_base_component_t *best_component = NULL;
    prte_pstat_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PRTE_SUCCESS != prte_mca_base_select("pstat", prte_pstat_base_framework.framework_output,
                                                &prte_pstat_base_framework.framework_components,
                                                (prte_mca_base_module_t **) &best_module,
                                                (prte_mca_base_component_t **) &best_component, NULL) ) {
        /* It is okay if we don't find a runnable component - default
         * to the unsupported default.
         */
        goto cleanup;
    }

    /* Save the winner */
    prte_pstat_base_component = best_component;
    prte_pstat                = *best_module;

    /* Initialize the winner */
    if (PRTE_SUCCESS != (ret = prte_pstat.init()) ) {
        exit_status = ret;
        goto cleanup;
    }

 cleanup:
    return exit_status;
}
