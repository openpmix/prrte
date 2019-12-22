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
 * Copyright (c) 2007-2008 Cisco Systems, Inc. All rights reserved.
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
#include "src/mca/pstat/pstat.h"
#include "pstat_linux.h"

/*
 * Public string showing the pstat ompi_linux component version number
 */
const char *prrte_pstat_linux_component_version_string =
    "PRRTE linux pstat MCA component version " PRRTE_VERSION;

/*
 * Local function
 */
static int pstat_linux_component_query(prrte_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

const prrte_pstat_base_component_t prrte_pstat_linux_component = {

    /* First, the mca_component_t struct containing meta information
       about the component itself */

    .base_version = {
        PRRTE_PSTAT_BASE_VERSION_2_0_0,

        /* Component name and version */
        .mca_component_name = "linux",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),

        .mca_query_component = pstat_linux_component_query,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};


static int pstat_linux_component_query(prrte_mca_base_module_t **module, int *priority)
{
    *priority = 20;
    *module = (prrte_mca_base_module_t *)&prrte_pstat_linux_module;

    return PRRTE_SUCCESS;
}

