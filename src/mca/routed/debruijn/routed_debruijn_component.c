/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2008 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
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
#include "routed_debruijn.h"

static int prrte_routed_debruijn_component_query(prrte_mca_base_module_t **module, int *priority);

/**
 * component definition
 */
prrte_routed_component_t prrte_routed_debruijn_component = {
      /* First, the prrte_mca_base_component_t struct containing meta
         information about the component itself */

    .base_version = {
        PRRTE_ROUTED_BASE_VERSION_3_0_0,

        .mca_component_name = "debruijn",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                              PRRTE_RELEASE_VERSION),
        .mca_query_component = prrte_routed_debruijn_component_query
    },
    .base_data = {
        /* This component can be checkpointed */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int prrte_routed_debruijn_component_query(prrte_mca_base_module_t **module, int *priority)
{
    /* Debruijn shall be our default, especially for large systems. For smaller
     * systems, we will allow other options that have even fewer hops to
     * support wireup
     */
    *priority = 10;
    *module = (prrte_mca_base_module_t *) &prrte_routed_debruijn_module;
    return PRRTE_SUCCESS;
}
