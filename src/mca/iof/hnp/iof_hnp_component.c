/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/mca/base/base.h"
#include "src/util/output.h"
#include "src/event/event-internal.h"

#include "src/util/proc_info.h"

#include "src/mca/iof/base/base.h"
#include "iof_hnp.h"

/*
 * Local functions
 */
static int prrte_iof_hnp_open(void);
static int prrte_iof_hnp_close(void);
static int prrte_iof_hnp_query(prrte_mca_base_module_t **module, int *priority);

/*
 * Public string showing the iof hnp component version number
 */
const char *prrte_iof_hnp_component_version_string =
    "PRRTE hnp iof MCA component version " PRRTE_VERSION;

prrte_iof_hnp_component_t prrte_iof_hnp_component = {
    {
        /* First, the prrte_mca_base_component_t struct containing meta
         information about the component itself */

        .iof_version = {
            PRRTE_IOF_BASE_VERSION_2_0_0,

            .mca_component_name = "hnp",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),

            /* Component open, close, and query functions */
            .mca_open_component = prrte_iof_hnp_open,
            .mca_close_component = prrte_iof_hnp_close,
            .mca_query_component = prrte_iof_hnp_query,
        },
        .iof_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

/**
  * component open/close/init function
  */
static int prrte_iof_hnp_open(void)
{
    /* Nothing to do */
    return PRRTE_SUCCESS;
}


static int prrte_iof_hnp_close(void)
{
    return PRRTE_SUCCESS;
}

/**
 * Module query
 */

static int prrte_iof_hnp_query(prrte_mca_base_module_t **module, int *priority)
{
    /* if we are not the HNP, then don't use this module */
    if (!PRRTE_PROC_IS_MASTER && !PRRTE_PROC_IS_MASTER) {
        *priority = -1;
        *module = NULL;
        return PRRTE_ERROR;
    }

    *priority = 100;
    *module = (prrte_mca_base_module_t *) &prrte_iof_hnp_module;

    return PRRTE_SUCCESS;
}
