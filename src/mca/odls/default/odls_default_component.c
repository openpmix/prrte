/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <ctype.h>

#include "src/mca/base/base.h"
#include "src/mca/mca.h"

#include "src/mca/odls/base/odls_private.h"
#include "src/mca/odls/default/odls_default.h"
#include "src/mca/odls/odls.h"

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

prte_odls_base_component_t prte_odls_default_component = {
    /* First, the mca_component_t struct containing meta information
    about the component itself */
    .version = {
        PRTE_ODLS_BASE_VERSION_2_0_0,
        /* Component name and version */
        .mca_component_name = "default",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                    PRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = prte_odls_default_component_open,
        .mca_close_component = prte_odls_default_component_close,
        .mca_query_component = prte_odls_default_component_query,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

int prte_odls_default_component_open(void)
{
    return PRTE_SUCCESS;
}

int prte_odls_default_component_query(prte_mca_base_module_t **module, int *priority)
{
    /* the base open/select logic protects us against operation when
     * we are NOT in a daemon, so we don't have to check that here
     */

    /* we have built some logic into the configure.m4 file that checks
     * to see if we have "fork" support and only builds this component
     * if we do. Hence, we only get here if we CAN build - in which
     * case, we definitely should be considered for selection
     */
    *priority = 10; /* let others override us - we are the default */
    *module = (prte_mca_base_module_t *) &prte_odls_default_module;
    return PRTE_SUCCESS;
}

int prte_odls_default_component_close(void)
{
    return PRTE_SUCCESS;
}
