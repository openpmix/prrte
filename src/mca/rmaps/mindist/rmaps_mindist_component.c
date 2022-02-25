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
#include "src/mca/base/pmix_mca_base_var.h"

#include "rmaps_mindist.h"
#include "src/mca/rmaps/base/rmaps_private.h"

/*
 * Local functions
 */

static int prte_rmaps_mindist_open(void);
static int prte_rmaps_mindist_close(void);
static int prte_rmaps_mindist_query(pmix_mca_base_module_t **module, int *priority);
static int prte_rmaps_mindist_register(void);

static int my_priority = 20;

prte_rmaps_base_component_t prte_mca_rmaps_mindist_component = {
    PRTE_RMAPS_BASE_VERSION_2_0_0,

    .pmix_mca_component_name = "mindist",
    PMIX_MCA_BASE_MAKE_VERSION(component,
                               PRTE_MAJOR_VERSION,
                               PRTE_MINOR_VERSION,
                               PMIX_RELEASE_VERSION),
    .pmix_mca_open_component = prte_rmaps_mindist_open,
    .pmix_mca_close_component = prte_rmaps_mindist_close,
    .pmix_mca_query_component = prte_rmaps_mindist_query,
    .pmix_mca_register_component_params = prte_rmaps_mindist_register,
};

static int prte_rmaps_mindist_register(void)
{
    (void) pmix_mca_base_component_var_register(&prte_mca_rmaps_mindist_component,
                                                "priority",
                                                "Priority of the mindist rmaps component",
                                                PMIX_MCA_BASE_VAR_TYPE_INT, &my_priority);
    return PRTE_SUCCESS;
}

/**
 * component open/close/init function
 */
static int prte_rmaps_mindist_open(void)
{
    return PRTE_SUCCESS;
}

static int prte_rmaps_mindist_query(pmix_mca_base_module_t **module, int *priority)
{
    /* the RMAPS framework is -only- opened on HNP's,
     * so no need to check for that here
     */

    *priority = my_priority;
    *module = (pmix_mca_base_module_t *) &prte_rmaps_mindist_module;
    return PRTE_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int prte_rmaps_mindist_close(void)
{
    return PRTE_SUCCESS;
}
