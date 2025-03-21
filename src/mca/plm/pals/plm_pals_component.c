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
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * Copyright (c) 2023      Triad National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/pmix_mca_base_var.h"

#include "src/runtime/prte_globals.h"

#include "plm_pals.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"

/*
 * Public string showing the plm ompi_pals component version number
 */
const char *prte_mca_plm_pals_component_version_string
    = "PRTE pals plm MCA component version " PRTE_VERSION;

/*
 * Local functions
 */
static int plm_pals_register(void);
static int plm_pals_open(void);
static int plm_pals_close(void);
static int prte_mca_plm_pals_component_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

prte_mca_plm_pals_component_t prte_mca_plm_pals_component = {
    .super = {
        PRTE_PLM_BASE_VERSION_2_0_0,

        /* Component name and version */
        .pmix_mca_component_name = "pals",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_open_component = plm_pals_open,
        .pmix_mca_close_component = plm_pals_close,
        .pmix_mca_query_component = prte_mca_plm_pals_component_query,
        .pmix_mca_register_component_params = plm_pals_register,
    }
};
PMIX_MCA_BASE_COMPONENT_INIT(prte, plm, pals)

static int plm_pals_register(void)
{
    pmix_mca_base_component_t *comp = &prte_mca_plm_pals_component.super;

    prte_mca_plm_pals_component.debug = false;
    (void) pmix_mca_base_component_var_register(comp, "debug", "Enable debugging of pals plm",
                                                PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                                &prte_mca_plm_pals_component.debug);

    if (prte_mca_plm_pals_component.debug == 0) {
        prte_mca_plm_pals_component.debug = prte_debug_flag;
    }

    prte_mca_plm_pals_component.priority = 100;
    (void) pmix_mca_base_component_var_register(comp, "priority", "Default selection priority",
                                                PMIX_MCA_BASE_VAR_TYPE_INT,
                                                &prte_mca_plm_pals_component.priority);

    prte_mca_plm_pals_component.aprun_cmd = "aprun";
    (void) pmix_mca_base_component_var_register(comp, "aprun", "Command to run instead of aprun",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_plm_pals_component.aprun_cmd);

    prte_mca_plm_pals_component.custom_args = NULL;
    (void) pmix_mca_base_component_var_register(comp, "args", "Custom arguments to aprun",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &prte_mca_plm_pals_component.custom_args);
    return PRTE_SUCCESS;
}

static int plm_pals_open(void)
{
    return PRTE_SUCCESS;
}

static int prte_mca_plm_pals_component_query(pmix_mca_base_module_t **module, int *priority)
{
    *priority = prte_mca_plm_pals_component.priority;
    *module = (pmix_mca_base_module_t *) &prte_plm_pals_module;
    PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                         "%s plm:pals: available for selection",
                         PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    return PRTE_SUCCESS;
}

static int plm_pals_close(void)
{
    return PRTE_SUCCESS;
}
