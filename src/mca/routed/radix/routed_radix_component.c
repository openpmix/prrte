/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2008 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"

#include "routed_radix.h"
#include "src/mca/routed/base/base.h"

static int prte_routed_radix_component_register(void);
static int prte_routed_radix_component_query(prte_mca_base_module_t **module, int *priority);

/**
 * component definition
 */
prte_routed_radix_component_t prte_routed_radix_component = {
    {
        /* First, the prte_mca_base_component_t struct containing meta
        information about the component itself */

        .base_version = {
            PRTE_ROUTED_BASE_VERSION_3_0_0,

            .mca_component_name = "radix",
            PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                        PRTE_RELEASE_VERSION),
            .mca_query_component = prte_routed_radix_component_query,
            .mca_register_component_params = prte_routed_radix_component_register,
        },
        .base_data = {
            /* This component can be checkpointed */
            PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

static int prte_routed_radix_component_register(void)
{
    prte_mca_base_component_t *c = &prte_routed_radix_component.super.base_version;

    prte_routed_radix_component.radix = 64;
    (void) prte_mca_base_component_var_register(c, NULL, "Radix to be used for routed radix tree",
                                                PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &prte_routed_radix_component.radix);

    return PRTE_SUCCESS;
}

static int prte_routed_radix_component_query(prte_mca_base_module_t **module, int *priority)
{
    if (0 > prte_routed_radix_component.radix) {
        return PRTE_ERR_BAD_PARAM;
    }

    *priority = 70;
    *module = (prte_mca_base_module_t *) &prte_routed_radix_module;
    return PRTE_SUCCESS;
}
