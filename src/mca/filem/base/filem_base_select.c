/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 The Trustees of the University of Tennessee.
 *                         All rights reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC.
 *                         All rights reserved
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <string.h>

#include "constants.h"

#include "src/mca/mca.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"

#include "src/mca/filem/filem.h"
#include "src/mca/filem/base/base.h"


int prrte_filem_base_select(void)
{
    int exit_status = PRRTE_SUCCESS;
    prrte_filem_base_component_t *best_component = NULL;
    prrte_filem_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PRRTE_SUCCESS != prrte_mca_base_select("filem", prrte_filem_base_framework.framework_output,
                                                &prrte_filem_base_framework.framework_components,
                                                (prrte_mca_base_module_t **) &best_module,
                                                (prrte_mca_base_component_t **) &best_component, NULL) ) {
        /* It is okay to not select anything - we'll just retain
         * the default none module
         */
        return PRRTE_SUCCESS;
    }

    /* Save the winner */
    prrte_filem = *best_module;

    /* Initialize the winner */
    if (NULL != prrte_filem.filem_init) {
        if (PRRTE_SUCCESS != prrte_filem.filem_init()) {
            exit_status = PRRTE_ERROR;
        }
    }

    return exit_status;
}
