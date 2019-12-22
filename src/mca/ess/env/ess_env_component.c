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
 *
 * These symbols are in a file by themselves to provide nice linker
 * semantics.  Since linkers generally pull in symbols by object
 * files, keeping these symbols as the only symbols in this file
 * prevents utility programs such as "ompi_info" from having to import
 * entire components just to query their version and parameters.
 */

#include "prrte_config.h"
#include "constants.h"

#include "src/util/proc_info.h"

#include "src/mca/ess/ess.h"
#include "src/mca/ess/env/ess_env.h"

extern prrte_ess_base_module_t prrte_ess_env_module;

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
prrte_ess_base_component_t prrte_ess_env_component = {
    .base_version = {
        PRRTE_ESS_BASE_VERSION_3_0_0,

        /* Component name and version */
        .mca_component_name = "env",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = prrte_ess_env_component_open,
        .mca_close_component = prrte_ess_env_component_close,
        .mca_query_component = prrte_ess_env_component_query,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};


int
prrte_ess_env_component_open(void)
{
    return PRRTE_SUCCESS;
}

int prrte_ess_env_component_query(prrte_mca_base_module_t **module, int *priority)
{
    /* we are the env module, only used by daemons that are
     * launched by ssh so allow any enviro-specifc modules
     * to override us */
    if (PRRTE_PROC_IS_DAEMON) {
        *priority = 1;
        *module = (prrte_mca_base_module_t *)&prrte_ess_env_module;
        return PRRTE_SUCCESS;
    }

    /* if not, then return NULL - we cannot be selected */
    *priority = -1;
    *module = NULL;
    return PRRTE_ERROR;
}


int
prrte_ess_env_component_close(void)
{
    return PRRTE_SUCCESS;
}

