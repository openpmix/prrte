/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2007 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
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
#include "src/util/basename.h"

#include "src/mca/ras/base/ras_private.h"
#include "ras_tm.h"


/*
 * Local variables
 */
static int param_priority;


/*
 * Local functions
 */
static int ras_tm_register(void);
static int ras_tm_open(void);
static int prrte_ras_tm_component_query(prrte_mca_base_module_t **module, int *priority);


prrte_ras_tm_component_t prrte_ras_tm_component = {
    {
        /* First, the prrte_mca_base_component_t struct containing meta
           information about the component itself */

        .base_version = {
            PRRTE_RAS_BASE_VERSION_2_0_0,

            /* Component name and version */
            .mca_component_name = "tm",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),

            /* Component open and close functions */
            .mca_open_component = ras_tm_open,
            .mca_query_component = prrte_ras_tm_component_query,
            .mca_register_component_params = ras_tm_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

static int ras_tm_register(void)
{
    prrte_mca_base_component_t *c        = &prrte_ras_tm_component.super.base_version;
    char *pbs_nodefile_env         = NULL;

    param_priority = 100;
    (void) prrte_mca_base_component_var_register(c, "priority", "Priority of the tm ras component",
                                           PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           PRRTE_INFO_LVL_9,
                                           PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                           &param_priority);

    prrte_ras_tm_component.nodefile_dir = NULL;

    /* try to detect the default directory */
    pbs_nodefile_env = getenv("PBS_NODEFILE");
    if (NULL != pbs_nodefile_env) {
        prrte_ras_tm_component.nodefile_dir = prrte_dirname(pbs_nodefile_env);
    }

    if (NULL == prrte_ras_tm_component.nodefile_dir) {
        prrte_ras_tm_component.nodefile_dir = strdup ("/var/torque/aux");
    }

    (void) prrte_mca_base_component_var_register (c, "nodefile_dir",
                                            "The directory where the PBS nodefile can be found",
                                            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_tm_component.nodefile_dir);

    /* for big SMP machines (e.g., those from SGI), listing the nodes
     * once/slot in the nodefile is extreme. In those cases, they may
     * choose to list each node once, but then provide an envar that
     * tells us how many cpus/node were allocated. Allow the user to
     * inform us that we are in such an environment
     */
    prrte_ras_tm_component.smp_mode = false;
    (void) prrte_mca_base_component_var_register (c, "smp",
                                            "The Torque system is configured in SMP mode "
                                            "with the number of cpus/node given in the environment",
                                            PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_tm_component.smp_mode);

    return PRRTE_SUCCESS;
}

static int ras_tm_open(void)
{
    return PRRTE_SUCCESS;
}


static int prrte_ras_tm_component_query(prrte_mca_base_module_t **module, int *priority)
{
    /* Are we running under a TM job? */
    if (NULL != getenv("PBS_ENVIRONMENT") &&
        NULL != getenv("PBS_JOBID")) {
        *priority = param_priority;
        *module = (prrte_mca_base_module_t *) &prrte_ras_tm_module;
        return PRRTE_SUCCESS;
    }

    /* Sadly, no */
    *module = NULL;
    return PRRTE_ERROR;
}
