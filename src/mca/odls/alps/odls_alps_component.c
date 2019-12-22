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
 * Copyright (c) 2014-2015 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
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

#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <ctype.h>
#include <sys/syscall.h>

#include "src/mca/mca.h"
#include "src/mca/base/base.h"

#include "src/mca/common/alps/common_alps.h"
#include "src/mca/odls/odls.h"
#include "src/mca/odls/base/odls_private.h"
#include "src/mca/odls/alps/odls_alps.h"

/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

prrte_odls_base_component_t prrte_odls_alps_component = {
    /* First, the mca_component_t struct containing meta information
    about the component itself */
    .version = {
        PRRTE_ODLS_BASE_VERSION_2_0_0,
        /* Component name and version */
        .mca_component_name = "alps",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = prrte_odls_alps_component_open,
        .mca_close_component = prrte_odls_alps_component_close,
        .mca_query_component = prrte_odls_alps_component_query,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};


int prrte_odls_alps_component_open(void)
{
    return PRRTE_SUCCESS;
}

int prrte_odls_alps_component_query(prrte_mca_base_module_t **module, int *priority)
{
    int rc = PRRTE_SUCCESS;
    bool flag;

    /*
     * make sure we're in a daemon process
     */

    if (!PRRTE_PROC_IS_DAEMON) {
        *priority = 0;
        *module = NULL;
        rc = PRRTE_ERROR;
    }

    /*
     * make sure we're in a Cray PAGG container, and that we are also on
     * a compute node (i.e. we are thought of as a application task by
     * the cray job kernel module  - the thing that creates the PAGG
     */

    rc = prrte_common_alps_proc_in_pagg(&flag);
    if ((PRRTE_SUCCESS == rc) && flag) {
        *priority = 80; /* take precendence over base and default */
        *module = (prrte_mca_base_module_t *) &prrte_odls_alps_module;
    }

    return rc;
}


int prrte_odls_alps_component_close(void)
{
    return PRRTE_SUCCESS;
}


