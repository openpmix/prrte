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
#include "src/mca/ess/slurm/ess_slurm.h"

extern prrte_ess_base_module_t prrte_ess_slurm_module;

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
prrte_ess_base_component_t prrte_ess_slurm_component = {
    .base_version = {
        PRRTE_ESS_BASE_VERSION_3_0_0,

        /* Component name and version */
        .mca_component_name = "slurm",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = prrte_ess_slurm_component_open,
        .mca_close_component = prrte_ess_slurm_component_close,
        .mca_query_component = prrte_ess_slurm_component_query,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};


int
prrte_ess_slurm_component_open(void)
{
    return PRRTE_SUCCESS;
}


int prrte_ess_slurm_component_query(prrte_mca_base_module_t **module, int *priority)
{
    /* Are we running under a SLURM job? Were
     * we given a path back to the HNP? If the
     * answer to both is "yes", then we were launched
     * by mpirun in a slurm world, so make ourselves available
     */

    if (PRRTE_PROC_IS_DAEMON &&
        NULL != getenv("SLURM_JOBID") &&
        NULL != prrte_process_info.my_hnp_uri) {
        *priority = 50;
        *module = (prrte_mca_base_module_t *)&prrte_ess_slurm_module;
        return PRRTE_SUCCESS;
    }

    /* Sadly, no */
    *priority = -1;
    *module = NULL;
    return PRRTE_ERROR;
}


int
prrte_ess_slurm_component_close(void)
{
    return PRRTE_SUCCESS;
}

