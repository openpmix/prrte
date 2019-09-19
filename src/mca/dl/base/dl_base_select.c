/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 *
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#ifdef HAVE_UNISTD_H
#include "unistd.h"
#endif

#include "src/include/constants.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/mca/dl/dl.h"
#include "src/mca/dl/base/base.h"


int prrte_dl_base_select(void)
{
    int exit_status = PRRTE_SUCCESS;
    prrte_dl_base_component_t *best_component = NULL;
    prrte_dl_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if (PRRTE_SUCCESS != prrte_mca_base_select("dl",
                                                prrte_dl_base_framework.framework_output,
                                                &prrte_dl_base_framework.framework_components,
                                                (prrte_mca_base_module_t **) &best_module,
                                                (prrte_mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected */
        exit_status = PRRTE_ERROR;
        goto cleanup;
    }

    /* Save the winner */
    prrte_dl_base_selected_component = best_component;
    prrte_dl = best_module;

 cleanup:
    return exit_status;
}
