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
 * Copyright (c) 2007      Cisco Systems, Inc. All rights reserved.
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
#include "src/mca/pstat/pstat.h"
#include "src/mca/pstat/base/base.h"

/*
 * Globals
 */

int prrte_pstat_base_select(void)
{
    int ret, exit_status = PRRTE_SUCCESS;
    prrte_pstat_base_component_t *best_component = NULL;
    prrte_pstat_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PRRTE_SUCCESS != prrte_mca_base_select("pstat", prrte_pstat_base_framework.framework_output,
                                                &prrte_pstat_base_framework.framework_components,
                                                (prrte_mca_base_module_t **) &best_module,
                                                (prrte_mca_base_component_t **) &best_component, NULL) ) {
        /* It is okay if we don't find a runnable component - default
         * to the unsupported default.
         */
        goto cleanup;
    }

    /* Save the winner */
    prrte_pstat_base_component = best_component;
    prrte_pstat                = *best_module;

    /* Initialize the winner */
    if (PRRTE_SUCCESS != (ret = prrte_pstat.init()) ) {
        exit_status = ret;
        goto cleanup;
    }

 cleanup:
    return exit_status;
}
