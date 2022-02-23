/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2004-2008 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
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
#include "constants.h"

#include "src/mca/base/pmix_base.h"

#include "src/runtime/prte_globals.h"
#include "src/util/proc_info.h"

#include "routed_direct.h"
#include "src/mca/routed/base/base.h"

static int mca_routed_direct_component_query(pmix_mca_base_module_t **module, int *priority);

/**
 * component definition
 */
prte_routed_component_t mca_routed_direct_component = {
    PRTE_ROUTED_BASE_VERSION_3_0_0,

    .pmix_mca_component_name = "direct",
    PMIX_MCA_BASE_MAKE_VERSION(component,
                               PRTE_MAJOR_VERSION,
                               PRTE_MINOR_VERSION,
                               PMIX_RELEASE_VERSION),
    .pmix_mca_query_component = mca_routed_direct_component_query
};

static int mca_routed_direct_component_query(pmix_mca_base_module_t **module, int *priority)
{
    /* allow selection only when specifically requested */
    *priority = 0;
    *module = (pmix_mca_base_module_t *) &prte_routed_direct_module;
    return PRTE_SUCCESS;
}
