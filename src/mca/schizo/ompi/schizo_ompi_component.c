/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "types.h"

#include "src/util/pmix_show_help.h"

#include "src/runtime/prte_globals.h"

#include "schizo_ompi.h"
#include "src/mca/schizo/schizo.h"

static int component_query(prte_mca_base_module_t **module, int *priority);
static int component_register(void);

/*
 * Struct of function pointers and all that to let us be initialized
 */
prte_schizo_ompi_component_t prte_schizo_ompi_component = {
    .super = {
        .base_version = {
            PRTE_MCA_SCHIZO_BASE_VERSION_1_0_0,
            .mca_component_name = "ompi",
            PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                        PMIX_RELEASE_VERSION),
            .mca_query_component = component_query,
            .mca_register_component_params = component_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    },
    .priority = 50,
    .warn_deprecations = false
};

static int component_register(void)
{
    prte_mca_base_component_t *c = &prte_schizo_ompi_component.super.base_version;

    prte_schizo_ompi_component.warn_deprecations = false;
    (void) prte_mca_base_component_var_register(c, "warn_deprecations",
                                                "Issue warnings about deprecated command line options",
                                                PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &prte_schizo_ompi_component.warn_deprecations);

    return PRTE_SUCCESS;
}

static int component_query(prte_mca_base_module_t **module, int *priority)
{
    *module = (prte_mca_base_module_t *) &prte_schizo_ompi_module;
    *priority = prte_schizo_ompi_component.priority;
    return PRTE_SUCCESS;
}
