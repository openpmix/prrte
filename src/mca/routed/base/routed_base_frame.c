/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2007-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2004-2010 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/class/prte_bitmap.h"
#include "src/mca/base/prte_mca_base_component_repository.h"
#include "src/mca/mca.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prte_globals.h"
#include "src/util/proc_info.h"

#include "src/mca/routed/base/base.h"
#include "src/mca/routed/routed.h"

/* The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prte_mca_base_component_t struct. */
#include "src/mca/routed/base/static-components.h"

prte_routed_base_t prte_routed_base = {0};
prte_routed_module_t prte_routed = {0};

static int prte_routed_base_open(prte_mca_base_open_flag_t flags)
{
    /* start with routing DISABLED */
    prte_routed_base.routing_enabled = false;

    /* Open up all available components */
    return prte_mca_base_framework_components_open(&prte_routed_base_framework, flags);
}

static int prte_routed_base_close(void)
{
    prte_routed_base.routing_enabled = false;
    if (NULL != prte_routed.finalize) {
        prte_routed.finalize();
    }
    return prte_mca_base_framework_components_close(&prte_routed_base_framework, NULL);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, routed, "PRTE Message Routing Subsystem", NULL,
                                prte_routed_base_open, prte_routed_base_close,
                                prte_routed_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);

int prte_routed_base_select(void)
{
    prte_routed_component_t *best_component = NULL;
    prte_routed_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if (PRTE_SUCCESS
        != prte_mca_base_select("routed", prte_routed_base_framework.framework_output,
                                &prte_routed_base_framework.framework_components,
                                (prte_mca_base_module_t **) &best_module,
                                (prte_mca_base_component_t **) &best_component, NULL)) {
        /* This will only happen if no component was selected */
        /* If we didn't find one to select, that is an error */
        return PRTE_ERROR;
    }

    /* Save the winner */
    prte_routed = *best_module;
    if (NULL != prte_routed.initialize) {
        prte_routed.initialize();
    }
    return PRTE_SUCCESS;
}

static void construct(prte_routed_tree_t *rt)
{
    rt->rank = PMIX_RANK_INVALID;
    PRTE_CONSTRUCT(&rt->relatives, prte_bitmap_t);
}
static void destruct(prte_routed_tree_t *rt)
{
    PRTE_DESTRUCT(&rt->relatives);
}
PRTE_CLASS_INSTANCE(prte_routed_tree_t, prte_list_item_t, construct, destruct);
