/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2008 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2013      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"

#include "src/mca/routed/base/base.h"
#include "routed_radix.h"

static int prrte_routed_radix_component_register(void);
static int prrte_routed_radix_component_query(prrte_mca_base_module_t **module, int *priority);

/**
 * component definition
 */
prrte_routed_radix_component_t prrte_routed_radix_component = {
    {
        /* First, the prrte_mca_base_component_t struct containing meta
        information about the component itself */

        .base_version = {
            PRRTE_ROUTED_BASE_VERSION_3_0_0,

            .mca_component_name = "radix",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),
            .mca_query_component = prrte_routed_radix_component_query,
            .mca_register_component_params = prrte_routed_radix_component_register,
        },
        .base_data = {
            /* This component can be checkpointed */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

static int prrte_routed_radix_component_register(void)
{
    prrte_mca_base_component_t *c = &prrte_routed_radix_component.super.base_version;

    prrte_routed_radix_component.radix = 64;
    (void) prrte_mca_base_component_var_register(c, NULL,
                                           "Radix to be used for routed radix tree",
                                           PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           PRRTE_INFO_LVL_9,
                                           PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                           &prrte_routed_radix_component.radix);

    return PRRTE_SUCCESS;
}

static int prrte_routed_radix_component_query(prrte_mca_base_module_t **module, int *priority)
{
    if (0 > prrte_routed_radix_component.radix) {
        return PRRTE_ERR_BAD_PARAM;
    }

    *priority = 70;
    *module = (prrte_mca_base_module_t *) &prrte_routed_radix_module;
    return PRRTE_SUCCESS;
}
