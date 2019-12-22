/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
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

#include "src/mca/rmaps/base/rmaps_private.h"
#include "rmaps_rr.h"

/*
 * Local functions
 */

static int prrte_rmaps_round_robin_register(void);
static int prrte_rmaps_round_robin_open(void);
static int prrte_rmaps_round_robin_close(void);
static int prrte_rmaps_round_robin_query(prrte_mca_base_module_t **module, int *priority);

static int my_priority;

prrte_rmaps_base_component_t prrte_rmaps_round_robin_component = {
    .base_version = {
        PRRTE_RMAPS_BASE_VERSION_2_0_0,

        .mca_component_name = "round_robin",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),
        .mca_open_component = prrte_rmaps_round_robin_open,
        .mca_close_component = prrte_rmaps_round_robin_close,
        .mca_query_component = prrte_rmaps_round_robin_query,
        .mca_register_component_params = prrte_rmaps_round_robin_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};


/**
  * component register/open/close/init function
  */
static int prrte_rmaps_round_robin_register(void)
{
    my_priority = 10;
    (void) prrte_mca_base_component_var_register(&prrte_rmaps_round_robin_component.base_version,
                                           "priority", "Priority of the rr rmaps component",
                                           PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           PRRTE_INFO_LVL_9,
                                           PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    return PRRTE_SUCCESS;
}

static int prrte_rmaps_round_robin_open(void)
{
    return PRRTE_SUCCESS;
}


static int prrte_rmaps_round_robin_query(prrte_mca_base_module_t **module, int *priority)
{
    /* the RMAPS framework is -only- opened on HNP's,
     * so no need to check for that here
     */

    *priority = my_priority;
    *module = (prrte_mca_base_module_t *)&prrte_rmaps_round_robin_module;
    return PRRTE_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int prrte_rmaps_round_robin_close(void)
{
    return PRRTE_SUCCESS;
}


