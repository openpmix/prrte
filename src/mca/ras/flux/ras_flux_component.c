/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2007 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2025-2026 Triad National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <flux/core.h>
#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_var.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "ras_flux.h"
#include "src/mca/ras/base/base.h"

/*
 * Local variables
 */
static int param_priority;

/*
 * Local functions
 */
static int ras_flux_register(void);
static int ras_flux_open(void);
static int prte_mca_ras_flux_component_query(pmix_mca_base_module_t **module, int *priority);

prte_mca_ras_flux_component_t prte_mca_ras_flux_component = {
    .super = {
        PRTE_RAS_BASE_VERSION_2_0_0,

        /* Component name and version */
        .pmix_mca_component_name = "flux",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_open_component = ras_flux_open,
        .pmix_mca_query_component = prte_mca_ras_flux_component_query,
        .pmix_mca_register_component_params = ras_flux_register,
    }
};
PMIX_MCA_BASE_COMPONENT_INIT(prte, ras, flux)

static int ras_flux_register(void)
{
    pmix_mca_base_component_t *c = &prte_mca_ras_flux_component.super;

    param_priority = 100;
    (void) pmix_mca_base_component_var_register(c, "priority", "Priority of the flux ras component",
                                                PMIX_MCA_BASE_VAR_TYPE_INT, &param_priority);

    prte_mca_ras_flux_component.flux_broker_uri = NULL;
    (void) pmix_mca_base_component_var_register(c, "broker_uri", "Flux broker URI to use",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_ras_flux_component.flux_broker_uri);

    prte_mca_ras_flux_component.flux_open_flags = 0;
    (void) pmix_mca_base_component_var_register(c, "open_flags", "Flux open flags",
                                                PMIX_MCA_BASE_VAR_TYPE_INT, 
                                                &prte_mca_ras_flux_component.flux_open_flags);
    return PRTE_SUCCESS;
}

static int ras_flux_open(void)
{
    return PRTE_SUCCESS;
}

static int prte_mca_ras_flux_component_query(pmix_mca_base_module_t **module, int *priority)
{
    flux_t *h = NULL;
    flux_error_t error;

    /* See if we can contact a Flux broker */
    h = flux_open_ex(prte_mca_ras_flux_component.flux_broker_uri, 
                     prte_mca_ras_flux_component.flux_open_flags, 
                     &error);
    if (NULL != h) {
        *priority = param_priority;
        *module = (pmix_mca_base_module_t *) &prte_ras_flux_module;
        flux_close(h);

        PMIX_OUTPUT_VERBOSE((2, prte_ras_base_framework.framework_output,
                             "%s ras:flux: available for selection",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        return PRTE_SUCCESS;
    }
    /* Sadly, no */
    *module = NULL;
    return PRTE_ERROR;
}
