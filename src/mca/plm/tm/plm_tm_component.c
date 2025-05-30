/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2022 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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
#include "src/util/pmix_argv.h"

#include "plm_tm.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"

/*
 * Public string showing the plm ompi_tm component version number
 */
const char *prte_mca_plm_tm_component_version_string
    = "PRTE tm plm MCA component version " PRTE_VERSION;

/*
 * Local function
 */
static int plm_tm_register(void);
static int plm_tm_open(void);
static int plm_tm_close(void);
static int prte_mca_plm_tm_component_query(pmix_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

prte_mca_plm_tm_component_t prte_mca_plm_tm_component = {
    .super = {
        PRTE_PLM_BASE_VERSION_2_0_0,

        /* Component name and version */
        .pmix_mca_component_name = "tm",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_open_component = plm_tm_open,
        .pmix_mca_close_component = plm_tm_close,
        .pmix_mca_query_component = prte_mca_plm_tm_component_query,
        .pmix_mca_register_component_params = plm_tm_register,
    }
};
PMIX_MCA_BASE_COMPONENT_INIT(prte, plm, tm)

static int plm_tm_register(void)
{
    pmix_mca_base_component_t *comp = &prte_mca_plm_tm_component.super;

    prte_mca_plm_tm_component.want_path_check = true;
    (void) pmix_mca_base_component_var_register(comp, "want_path_check",
                                                "Whether the launching process should check for the plm_tm_orted executable in the PATH "
                                                "before launching (the TM API does not give an indication of failure; this is a "
                                                "somewhat-lame workaround; non-zero values enable this check)",
                                                PMIX_MCA_BASE_VAR_TYPE_BOOL, &prte_mca_plm_tm_component.want_path_check);

    return PRTE_SUCCESS;
}

static int plm_tm_open(void)
{
    prte_mca_plm_tm_component.checked_paths = NULL;

    return PRTE_SUCCESS;
}

static int plm_tm_close(void)
{
    if (NULL != prte_mca_plm_tm_component.checked_paths) {
        PMIX_ARGV_FREE_COMPAT(prte_mca_plm_tm_component.checked_paths);
    }

    return PRTE_SUCCESS;
}

static int prte_mca_plm_tm_component_query(pmix_mca_base_module_t **module, int *priority)
{
    /* Are we running under a TM job? */

    if (NULL != getenv("PBS_ENVIRONMENT") && NULL != getenv("PBS_JOBID")) {

        *priority = 75;
        *module = (pmix_mca_base_module_t *) &prte_plm_tm_module;
        return PRTE_SUCCESS;
    }

    /* Sadly, no */
    *module = NULL;
    return PRTE_ERROR;
}
