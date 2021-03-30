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
 * Copyright (c) 2013-2015 Los Alamos National Security, LLC.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
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
#include "src/mca/base/prte_mca_base_var.h"

#include "rmaps_mindist.h"
#include "src/mca/rmaps/base/rmaps_private.h"

/*
 * Local functions
 */

static int prte_rmaps_mindist_open(void);
static int prte_rmaps_mindist_close(void);
static int prte_rmaps_mindist_query(prte_mca_base_module_t **module, int *priority);
static int prte_rmaps_mindist_register(void);

static int my_priority = 20;

prte_rmaps_base_component_t prte_rmaps_mindist_component = {
    .base_version = {
        PRTE_RMAPS_BASE_VERSION_2_0_0,

        .mca_component_name = "mindist",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                  PRTE_RELEASE_VERSION),
        .mca_open_component = prte_rmaps_mindist_open,
        .mca_close_component = prte_rmaps_mindist_close,
        .mca_query_component = prte_rmaps_mindist_query,
        .mca_register_component_params = prte_rmaps_mindist_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int prte_rmaps_mindist_register(void)
{
    (void) prte_mca_base_component_var_register(&prte_rmaps_mindist_component.base_version,
                                                "priority",
                                                "Priority of the mindist rmaps component",
                                                PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);
    return PRTE_SUCCESS;
}

/**
 * component open/close/init function
 */
static int prte_rmaps_mindist_open(void)
{
    return PRTE_SUCCESS;
}

static int prte_rmaps_mindist_query(prte_mca_base_module_t **module, int *priority)
{
    /* the RMAPS framework is -only- opened on HNP's,
     * so no need to check for that here
     */

    *priority = my_priority;
    *module = (prte_mca_base_module_t *) &prte_rmaps_mindist_module;
    return PRTE_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int prte_rmaps_mindist_close(void)
{
    return PRTE_SUCCESS;
}
