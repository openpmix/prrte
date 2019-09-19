/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012-2015 Los Alamos National Security, Inc.  All rights
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

#include <string.h>

#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/util/output.h"

#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"

int prrte_errmgr_base_select(void)
{
    int exit_status = PRRTE_SUCCESS;
    prrte_errmgr_base_component_t *best_component = NULL;
    prrte_errmgr_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PRRTE_SUCCESS != prrte_mca_base_select("errmgr", prrte_errmgr_base_framework.framework_output,
                                                &prrte_errmgr_base_framework.framework_components,
                                                (prrte_mca_base_module_t **) &best_module,
                                                (prrte_mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected */
        exit_status = PRRTE_ERROR;
        goto cleanup;
    }

    /* Save the winner */
    prrte_errmgr = *best_module;

    /* Initialize the winner */
    if (NULL != best_module) {
        if (PRRTE_SUCCESS != prrte_errmgr.init()) {
            exit_status = PRRTE_ERROR;
            goto cleanup;
        }
    }

 cleanup:
    return exit_status;
}
