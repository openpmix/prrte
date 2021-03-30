/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2008 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
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
#include "constants.h"

#include "src/mca/base/base.h"

#include "routed_debruijn.h"
#include "src/mca/routed/base/base.h"

static int prte_routed_debruijn_component_query(prte_mca_base_module_t **module, int *priority);

/**
 * component definition
 */
prte_routed_component_t prte_routed_debruijn_component = {
      /* First, the prte_mca_base_component_t struct containing meta
         information about the component itself */

    .base_version = {
        PRTE_ROUTED_BASE_VERSION_3_0_0,

        .mca_component_name = "debruijn",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                              PRTE_RELEASE_VERSION),
        .mca_query_component = prte_routed_debruijn_component_query
    },
    .base_data = {
        /* This component can be checkpointed */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int prte_routed_debruijn_component_query(prte_mca_base_module_t **module, int *priority)
{
    /* Debruijn shall be our default, especially for large systems. For smaller
     * systems, we will allow other options that have even fewer hops to
     * support wireup
     */
    *priority = 10;
    *module = (prte_mca_base_module_t *) &prte_routed_debruijn_module;
    return PRTE_SUCCESS;
}
