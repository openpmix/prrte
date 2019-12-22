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
 * Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
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

#include "prrte_config.h"
#include "constants.h"

#include <lsf/lsbatch.h>

#include "src/util/proc_info.h"

#include "src/mca/ess/ess.h"
#include "src/mca/ess/lsf/ess_lsf.h"

extern prrte_ess_base_module_t prrte_ess_lsf_module;

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */
prrte_ess_base_component_t prrte_ess_lsf_component = {
    .base_version = {
        PRRTE_ESS_BASE_VERSION_3_0_0,

        /* Component name and version */
        .mca_component_name = "lsf",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = prrte_ess_lsf_component_open,
        .mca_close_component = prrte_ess_lsf_component_close,
        .mca_query_component = prrte_ess_lsf_component_query,
    },
    .base_data = {
        /* The component is not checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_NONE
    },
};


int prrte_ess_lsf_component_open(void)
{
    return PRRTE_SUCCESS;
}


int prrte_ess_lsf_component_query(prrte_mca_base_module_t **module, int *priority)
{
    /* Are we running under an LSF job? Were
     * we given a path back to the HNP? If the
     * answer to both is "yes", then we were launched
     * by mpirun in an LSF world
     */

    if (PRRTE_PROC_IS_DAEMON &&
        NULL != getenv("LSB_JOBID") &&
        NULL != prrte_process_info.my_hnp_uri) {
        *priority = 40;
        *module = (prrte_mca_base_module_t *)&prrte_ess_lsf_module;
        return PRRTE_SUCCESS;
    }

    /* nope, not here */
    *priority = -1;
    *module = NULL;
    return PRRTE_ERROR;
}


int prrte_ess_lsf_component_close(void)
{
    return PRRTE_SUCCESS;
}

