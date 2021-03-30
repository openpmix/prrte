/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "types.h"

#include "src/util/show_help.h"

#include "src/runtime/prte_globals.h"

#include "schizo_prte.h"
#include "src/mca/schizo/schizo.h"

static int component_query(prte_mca_base_module_t **module, int *priority);

/*
 * Struct of function pointers and all that to let us be initialized
 */
prte_schizo_prte_component_t prte_schizo_prte_component = {
    .super = {
        .base_version = {
            PRTE_MCA_SCHIZO_BASE_VERSION_1_0_0,
            .mca_component_name = "prte",
            PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                        PRTE_RELEASE_VERSION),
            .mca_query_component = component_query,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    },
    .priority = 5
};

static int component_query(prte_mca_base_module_t **module, int *priority)
{
    *module = (prte_mca_base_module_t *) &prte_schizo_prte_module;
    *priority = prte_schizo_prte_component.priority;
    return PRTE_SUCCESS;
}
