/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
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

#include "src/util/show_help.h"

#include "src/mca/rmaps/base/base.h"
#include "rmaps_ppr.h"

/*
 * Local functions
 */

static int prrte_rmaps_ppr_open(void);
static int prrte_rmaps_ppr_close(void);
static int prrte_rmaps_ppr_query(prrte_mca_base_module_t **module, int *priority);
static int prrte_rmaps_ppr_register(void);

prrte_rmaps_base_component_t prrte_rmaps_ppr_component = {
    .base_version = {
        PRRTE_RMAPS_BASE_VERSION_2_0_0,

        .mca_component_name = "ppr",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),
        .mca_open_component = prrte_rmaps_ppr_open,
        .mca_close_component = prrte_rmaps_ppr_close,
        .mca_query_component = prrte_rmaps_ppr_query,
        .mca_register_component_params = prrte_rmaps_ppr_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int my_priority;

static int prrte_rmaps_ppr_open(void)
{
    return PRRTE_SUCCESS;
}


static int prrte_rmaps_ppr_query(prrte_mca_base_module_t **module, int *priority)
{
    *priority = my_priority;
    *module = (prrte_mca_base_module_t *)&prrte_rmaps_ppr_module;
    return PRRTE_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int prrte_rmaps_ppr_close(void)
{
    return PRRTE_SUCCESS;
}


static int prrte_rmaps_ppr_register(void)
{
    my_priority = 90;
    (void) prrte_mca_base_component_var_register(&prrte_rmaps_ppr_component.base_version,
                                           "priority", "Priority of the ppr rmaps component",
                                           PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           PRRTE_INFO_LVL_9,
                                           PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    return PRRTE_SUCCESS;
}
