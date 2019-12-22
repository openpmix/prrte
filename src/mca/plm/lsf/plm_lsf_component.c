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
 * Copyright (c) 2006-2007 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2008      Institut National de Recherche en Informatique
 *                         et Automatique. All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
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

#include <lsf/lsbatch.h>

#include "src/util/output.h"


#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "plm_lsf.h"


/*
 * Public string showing the plm lsf component version number
 */
const char *prrte_plm_lsf_component_version_string =
  "PRRTE lsf plm MCA component version " PRRTE_VERSION;



/*
 * Local function
 */
static int plm_lsf_open(void);
static int plm_lsf_close(void);
static int prrte_plm_lsf_component_query(prrte_mca_base_module_t **module, int *priority);


/*
 * Instantiate the public struct with all of our public information
 * and pointers to our public functions in it
 */

prrte_plm_lsf_component_t prrte_plm_lsf_component = {
    {
        /* First, the mca_component_t struct containing meta information
           about the component itself */

        .base_version = {
            PRRTE_PLM_BASE_VERSION_2_0_0,

            /* Component name and version */
            .mca_component_name = "lsf",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),

            /* Component open and close functions */
            .mca_open_component = plm_lsf_open,
            .mca_close_component = plm_lsf_close,
            .mca_query_component = prrte_plm_lsf_component_query,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};


static int plm_lsf_open(void)
{
    return PRRTE_SUCCESS;
}


static int plm_lsf_close(void)
{
    return PRRTE_SUCCESS;
}


static int prrte_plm_lsf_component_query(prrte_mca_base_module_t **module, int *priority)
{

    /* check if lsf is running here and make sure IBM CSM is NOT enabled */
    if (NULL == getenv("LSB_JOBID") || getenv("CSM_ALLOCATION_ID") || lsb_init("PRRTE launcher") < 0) {
        /* nope, not here */
        prrte_output_verbose(10, prrte_plm_base_framework.framework_output,
                            "plm:lsf: NOT available for selection");
        *module = NULL;
        return PRRTE_ERROR;
    }

    *priority = 75;
    *module = (prrte_mca_base_module_t *) &prrte_plm_lsf_module;
    return PRRTE_SUCCESS;
}
