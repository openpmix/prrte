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
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
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
#include "src/util/name_fns.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_show_help.h"

#include "plm_slurm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"

/*
 * Public string showing the plm ompi_slurm component version number
 */
const char *prte_mca_plm_slurm_component_version_string
    = "PRTE slurm plm MCA component version " PRTE_VERSION;

/*
 * Local functions
 */
static int plm_slurm_register(void);
static int plm_slurm_open(void);
static int plm_slurm_close(void);
static int prte_mca_plm_slurm_component_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

prte_mca_plm_slurm_component_t prte_mca_plm_slurm_component = {
    .super = {
        PRTE_PLM_BASE_VERSION_2_0_0,

        /* Component name and version */
        .pmix_mca_component_name = "slurm",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_open_component = plm_slurm_open,
        .pmix_mca_close_component = plm_slurm_close,
        .pmix_mca_query_component = prte_mca_plm_slurm_component_query,
        .pmix_mca_register_component_params = plm_slurm_register,
    }

    /* Other prte_mca_plm_slurm_component_t items -- left uninitialized
       here; will be initialized in plm_slurm_open() */
};

static char *custom_args = NULL;
static char *force_args = NULL;

static int plm_slurm_register(void)
{
    pmix_mca_base_component_t *comp = &prte_mca_plm_slurm_component.super;


    prte_mca_plm_slurm_component.custom_args_index =
            pmix_mca_base_component_var_register(comp, "args", "Custom arguments to srun",
                                                 PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                 &custom_args);

    force_args = NULL;
    (void) pmix_mca_base_component_var_register(comp, "force_args", "Mandatory custom arguments to srun",
                                                PMIX_MCA_BASE_VAR_TYPE_STRING,
                                                &force_args);

    prte_mca_plm_slurm_component.slurm_warning_msg = false;
    (void) pmix_mca_base_component_var_register(comp, "disable_warning", "Turn off warning message about custom args set in environment",
                                                PMIX_MCA_BASE_VAR_TYPE_BOOL,
                                                &prte_mca_plm_slurm_component.slurm_warning_msg);

    return PRTE_SUCCESS;
}

static int plm_slurm_open(void)
{
    return PRTE_SUCCESS;
}

static int prte_mca_plm_slurm_component_query(pmix_mca_base_module_t **module, int *priority)
{
    const pmix_mca_base_var_t *var;
    pmix_status_t rc;

    /* Are we running under a SLURM job? */

    if (NULL != getenv("SLURM_JOBID")) {
        *priority = 75;

        PMIX_OUTPUT_VERBOSE((1, prte_plm_base_framework.framework_output,
                             "%s plm:slurm: available for selection",
                             PRTE_NAME_PRINT(PRTE_PROC_MY_NAME)));

    prte_mca_plm_slurm_component.custom_args = NULL;

        // if we were are warning about externally set custom args, then
        // check to see if that was done
        if (!prte_mca_plm_slurm_component.slurm_warning_msg &&
            NULL == force_args) {
            // check for custom args
            rc = pmix_mca_base_var_get(prte_mca_plm_slurm_component.custom_args_index, &var);
            if (PMIX_SUCCESS == rc) {
                // the variable was set - see who set it
                if (PMIX_MCA_BASE_VAR_SOURCE_ENV == var->mbv_source) {
                    // set in the environment - warn
                    pmix_show_help("help-plm-slurm.txt", "custom-args-in-env", true,
                                   custom_args);
                }
            }
        }

        if (NULL != force_args) {
            prte_mca_plm_slurm_component.custom_args = force_args;
        } else if (NULL != custom_args) {
            prte_mca_plm_slurm_component.custom_args = custom_args;
        }

        *module = (pmix_mca_base_module_t *) &prte_plm_slurm_module;
        return PRTE_SUCCESS;
    }

    /* Sadly, no */
    *module = NULL;
    return PRTE_ERROR;
}

static int plm_slurm_close(void)
{
    return PRTE_SUCCESS;
}
