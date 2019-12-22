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

#include "prrte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"
#include "src/util/output.h"

#include "src/runtime/prrte_globals.h"
#include "src/util/name_fns.h"

#include "src/mca/ras/base/ras_private.h"
#include "ras_gridengine.h"

/*
 * Local functions
 */

static int prrte_ras_gridengine_register(void);
static int prrte_ras_gridengine_open(void);
static int prrte_ras_gridengine_close(void);
static int prrte_ras_gridengine_component_query(prrte_mca_base_module_t **module, int *priority);

static int prrte_ras_gridengine_verbose;

prrte_ras_gridengine_component_t prrte_ras_gridengine_component = {
    {
        /* First, the prrte_mca_base_component_t struct containing meta
           information about the component itself */

        .base_version = {
            PRRTE_RAS_BASE_VERSION_2_0_0,
            .mca_component_name = "gridengine",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),
            .mca_open_component = prrte_ras_gridengine_open,
            .mca_close_component = prrte_ras_gridengine_close,
            .mca_query_component = prrte_ras_gridengine_component_query,
            .mca_register_component_params = prrte_ras_gridengine_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

static int prrte_ras_gridengine_register(void)
{
    prrte_mca_base_component_t *c = &prrte_ras_gridengine_component.super.base_version;

    prrte_ras_gridengine_component.priority = 100;
    (void) prrte_mca_base_component_var_register (c, "priority", "Priority of the gridengine ras component",
                                            PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_ras_gridengine_component.priority);

    prrte_ras_gridengine_verbose = 0;
    (void) prrte_mca_base_component_var_register (c, "verbose",
                                            "Enable verbose output for the gridengine ras component",
                                            PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_LOCAL, &prrte_ras_gridengine_verbose);

    prrte_ras_gridengine_component.show_jobid = false;
    (void) prrte_mca_base_component_var_register (c, "show_jobid", "Show the JOB_ID of the Grid Engine job",
                                            PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0, PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_ras_gridengine_component.show_jobid);

    return PRRTE_SUCCESS;
}

/**
  * component open/close/init function
  */
static int prrte_ras_gridengine_open(void)
{
    if (prrte_ras_gridengine_verbose != 0) {
        prrte_ras_gridengine_component.verbose = prrte_output_open(NULL);
    } else {
        prrte_ras_gridengine_component.verbose = -1;
    }

    return PRRTE_SUCCESS;
}

static int prrte_ras_gridengine_component_query(prrte_mca_base_module_t **module, int *priority)
{
    *priority = prrte_ras_gridengine_component.priority;

    if (NULL != getenv("SGE_ROOT") && NULL != getenv("ARC") &&
        NULL != getenv("PE_HOSTFILE") && NULL != getenv("JOB_ID")) {
        PRRTE_OUTPUT_VERBOSE((2, prrte_ras_base_framework.framework_output,
                             "%s ras:gridengine: available for selection",
                             PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
        *module = (prrte_mca_base_module_t *) &prrte_ras_gridengine_module;
        return PRRTE_SUCCESS;
    }
    PRRTE_OUTPUT_VERBOSE((2, prrte_ras_base_framework.framework_output,
                         "%s ras:gridengine: NOT available for selection",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
    *module = NULL;
    return PRRTE_ERROR;
}

/**
 *  Close all subsystems.
 */
static int prrte_ras_gridengine_close(void)
{
    return PRRTE_SUCCESS;
}
