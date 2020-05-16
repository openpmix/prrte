/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 *
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#include "unistd.h"
#endif

#include "src/include/constants.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/mca/prtecompress/prtecompress.h"
#include "src/mca/prtecompress/base/base.h"

int prte_prtecompress_base_select(void)
{
    int ret = PRTE_SUCCESS;
    prte_prtecompress_base_component_t *best_component = NULL;
    prte_prtecompress_base_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PRTE_SUCCESS != prte_mca_base_select("prtecompress", prte_prtecompress_base_framework.framework_output,
                                               &prte_prtecompress_base_framework.framework_components,
                                               (prte_mca_base_module_t **) &best_module,
                                               (prte_mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected,
         * in which case we use the default one */
        goto cleanup;
    }

    /* Save the winner */
    prte_prtecompress_base_selected_component = *best_component;

    /* Initialize the winner */
    if (NULL != best_module) {
        if (PRTE_SUCCESS != (ret = best_module->init()) ) {
            goto cleanup;
        }
        prte_compress = *best_module;
    }

 cleanup:
    return ret;
}
