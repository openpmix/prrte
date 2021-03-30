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
 * Copyright (c) 2006      Sun Microsystems, Inc.  All rights reserved.
 *                         Use is subject to license terms.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
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
/**
 * @file
 * Resource allocation for Grid Engine
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"
#include "src/util/output.h"

#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "ras_gridengine.h"
#include "src/mca/ras/base/ras_private.h"

/*
 * Local functions
 */

static int prte_ras_gridengine_register(void);
static int prte_ras_gridengine_open(void);
static int prte_ras_gridengine_close(void);
static int prte_ras_gridengine_component_query(prte_mca_base_module_t **module, int *priority);

static int prte_ras_gridengine_verbose;

prte_ras_gridengine_component_t prte_ras_gridengine_component = {
    {
        /* First, the prte_mca_base_component_t struct containing meta
           information about the component itself */

        .base_version = {
            PRTE_RAS_BASE_VERSION_2_0_0,
            .mca_component_name = "gridengine",
            PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                        PRTE_RELEASE_VERSION),
            .mca_open_component = prte_ras_gridengine_open,
            .mca_close_component = prte_ras_gridengine_close,
            .mca_query_component = prte_ras_gridengine_component_query,
            .mca_register_component_params = prte_ras_gridengine_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

static int prte_ras_gridengine_register(void)
{
    prte_mca_base_component_t *c = &prte_ras_gridengine_component.super.base_version;

    prte_ras_gridengine_component.priority = 100;
    (void) prte_mca_base_component_var_register(c, "priority",
                                                "Priority of the gridengine ras component",
                                                PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &prte_ras_gridengine_component.priority);

    prte_ras_gridengine_verbose = 0;
    (void) prte_mca_base_component_var_register(
        c, "verbose", "Enable verbose output for the gridengine ras component",
        PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_LOCAL, &prte_ras_gridengine_verbose);

    prte_ras_gridengine_component.show_jobid = false;
    (void) prte_mca_base_component_var_register(c, "show_jobid",
                                                "Show the JOB_ID of the Grid Engine job",
                                                PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &prte_ras_gridengine_component.show_jobid);

    return PRTE_SUCCESS;
}

/**
 * component open/close/init function
 */
static int prte_ras_gridengine_open(void)
{
    if (prte_ras_gridengine_verbose != 0) {
        prte_ras_gridengine_component.verbose = prte_output_open(NULL);
    } else {
        prte_ras_gridengine_component.verbose = -1;
    }

    return PRTE_SUCCESS;
}

static int prte_ras_gridengine_component_query(prte_mca_base_module_t **module, int *priority)
{
    *priority = prte_ras_gridengine_component.priority;

    if (NULL != getenv("SGE_ROOT") && NULL != getenv("ARC") && NULL != getenv("PE_HOSTFILE")
        && NULL != getenv("JOB_ID")) {
        PRTE_OUTPUT_VERBOSE((2, prte_ras_base_framework.framework_output,
                             "%s ras:gridengine: available for selection",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
        *module = (prte_mca_base_module_t *) &prte_ras_gridengine_module;
        return PRTE_SUCCESS;
    }
    PRTE_OUTPUT_VERBOSE((2, prte_ras_base_framework.framework_output,
                         "%s ras:gridengine: NOT available for selection",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));
    *module = NULL;
    return PRTE_ERROR;
}

/**
 *  Close all subsystems.
 */
static int prte_ras_gridengine_close(void)
{
    return PRTE_SUCCESS;
}
