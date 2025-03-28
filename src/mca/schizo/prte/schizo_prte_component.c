/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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

#include "schizo_prte.h"
#include "src/mca/schizo/schizo.h"

static int component_query(pmix_mca_base_module_t **module, int *priority);
static int component_register(void);

/*
 * Struct of function pointers and all that to let us be initialized
 */
prte_schizo_prte_component_t prte_mca_schizo_prte_component = {
    .super = {
        PRTE_MCA_SCHIZO_BASE_VERSION_1_0_0,
        .pmix_mca_component_name = "prte",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),
        .pmix_mca_query_component = component_query,
        .pmix_mca_register_component_params = component_register,
    },
    .priority = 5,
    .warn_deprecations = true
};
PMIX_MCA_BASE_COMPONENT_INIT(prte, schizo, prte)

static int component_register(void)
{
    pmix_mca_base_component_t *c = &prte_mca_schizo_prte_component.super;

    prte_mca_schizo_prte_component.warn_deprecations = true;
    (void) pmix_mca_base_component_var_register(c, "warn_deprecations",
                                                "Issue warnings about deprecated command line options",
                                                PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                                &prte_mca_schizo_prte_component.warn_deprecations);

    return PRTE_SUCCESS;
}

static int component_query(pmix_mca_base_module_t **module, int *priority)
{
    *module = (pmix_mca_base_module_t *) &prte_schizo_prte_module;
    *priority = prte_mca_schizo_prte_component.priority;
    return PRTE_SUCCESS;
}
