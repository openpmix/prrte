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
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
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
#include "src/mca/base/prrte_mca_base_var.h"
#include "src/util/net.h"
#include "src/include/prrte_socket_errno.h"

#include "src/util/name_fns.h"
#include "src/mca/errmgr/errmgr.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/ras/base/ras_private.h"
#include "ras_slurm.h"


/*
 * Local functions
 */
static int ras_slurm_register(void);
static int ras_slurm_open(void);
static int ras_slurm_close(void);
static int prrte_ras_slurm_component_query(prrte_mca_base_module_t **module, int *priority);


prrte_ras_slurm_component_t prrte_ras_slurm_component = {
    {
        /* First, the prrte_mca_base_component_t struct containing meta
           information about the component itself */

        .base_version = {
            PRRTE_RAS_BASE_VERSION_2_0_0,

            /* Component name and version */
            .mca_component_name = "slurm",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),

            /* Component open and close functions */
            .mca_open_component = ras_slurm_open,
            .mca_close_component = ras_slurm_close,
            .mca_query_component = prrte_ras_slurm_component_query,
            .mca_register_component_params = ras_slurm_register
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};


static int ras_slurm_register(void)
{
    prrte_mca_base_component_t *component = &prrte_ras_slurm_component.super.base_version;

    prrte_ras_slurm_component.timeout = 30;
    (void) prrte_mca_base_component_var_register (component, "dyn_allocate_timeout",
                                            "Number of seconds to wait for Slurm dynamic allocation",
                                            PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_slurm_component.timeout);

    prrte_ras_slurm_component.dyn_alloc_enabled = false;
    (void) prrte_mca_base_component_var_register (component, "enable_dyn_alloc",
                                            "Whether or not dynamic allocations are enabled",
                                            PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_slurm_component.dyn_alloc_enabled);

    prrte_ras_slurm_component.config_file = NULL;
    (void) prrte_mca_base_component_var_register (component, "config_file",
                                            "Path to Slurm configuration file",
                                            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_slurm_component.config_file);

    prrte_ras_slurm_component.rolling_alloc = false;
    (void) prrte_mca_base_component_var_register (component, "enable_rolling_alloc",
                                            "Enable partial dynamic allocations",
                                            PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_slurm_component.rolling_alloc);

    prrte_ras_slurm_component.use_all = false;
    (void) prrte_mca_base_component_var_register (component, "use_entire_allocation",
                                            "Use entire allocation (not just job step nodes) for this application",
                                            PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                            PRRTE_INFO_LVL_5,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_slurm_component.use_all);

    return PRRTE_SUCCESS;
}

static int ras_slurm_open(void)
{
    return PRRTE_SUCCESS;
}

static int ras_slurm_close(void)
{
    return PRRTE_SUCCESS;
}

static int prrte_ras_slurm_component_query(prrte_mca_base_module_t **module, int *priority)
{
    /* if I built, then slurm support is available. If
     * I am not in a Slurm allocation, and dynamic alloc
     * is not enabled, then disqualify myself
     */
    if (NULL == getenv("SLURM_JOBID") &&
        !prrte_ras_slurm_component.dyn_alloc_enabled) {
        /* disqualify ourselves */
        *priority = 0;
        *module = NULL;
        return PRRTE_ERROR;
    }

    PRRTE_OUTPUT_VERBOSE((2, prrte_ras_base_framework.framework_output,
                         "%s ras:slurm: available for selection",
                         PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));
    /* since only one RM can exist on a cluster, just set
     * my priority to something - the other components won't
     * be responding anyway
     */
    *priority = 50;
    *module = (prrte_mca_base_module_t *) &prrte_ras_slurm_module;
    return PRRTE_SUCCESS;
}
