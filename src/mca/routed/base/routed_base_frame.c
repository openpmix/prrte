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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include "src/mca/mca.h"
#include "src/class/prrte_bitmap.h"
#include "src/util/output.h"
#include "src/mca/base/prrte_mca_base_component_repository.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/proc_info.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/routed/routed.h"
#include "src/mca/routed/base/base.h"


/* The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct. */
#include "src/mca/routed/base/static-components.h"

prrte_routed_base_t prrte_routed_base = {0};
prrte_routed_module_t prrte_routed = {0};

static int prrte_routed_base_open(prrte_mca_base_open_flag_t flags)
{
    /* start with routing DISABLED */
    prrte_routed_base.routing_enabled = false;

    /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_routed_base_framework, flags);
}

static int prrte_routed_base_close(void)
{
    prrte_routed_base.routing_enabled = false;
    if (NULL != prrte_routed.finalize) {
        prrte_routed.finalize();
    }
    return prrte_mca_base_framework_components_close(&prrte_routed_base_framework, NULL);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, routed, "PRRTE Message Routing Subsystem", NULL,
                                 prrte_routed_base_open, prrte_routed_base_close,
                                 prrte_routed_base_static_components, 0);

int prrte_routed_base_select(void)
{
    prrte_routed_component_t *best_component = NULL;
    prrte_routed_module_t *best_module = NULL;

    /*
     * Select the best component
     */
    if( PRRTE_SUCCESS != prrte_mca_base_select("routed", prrte_routed_base_framework.framework_output,
                                                &prrte_routed_base_framework.framework_components,
                                                (prrte_mca_base_module_t **) &best_module,
                                                (prrte_mca_base_component_t **) &best_component, NULL) ) {
        /* This will only happen if no component was selected */
        /* If we didn't find one to select, that is an error */
        return PRRTE_ERROR;
    }

    /* Save the winner */
    prrte_routed = *best_module;
    if (NULL != prrte_routed.initialize) {
        prrte_routed.initialize();
    }
    return PRRTE_SUCCESS;
}

static void construct(prrte_routed_tree_t *rt)
{
    rt->vpid = PRRTE_VPID_INVALID;
    PRRTE_CONSTRUCT(&rt->relatives, prrte_bitmap_t);
}
static void destruct(prrte_routed_tree_t *rt)
{
    PRRTE_DESTRUCT(&rt->relatives);
}
PRRTE_CLASS_INSTANCE(prrte_routed_tree_t,
                   prrte_list_item_t,
                   construct, destruct);
