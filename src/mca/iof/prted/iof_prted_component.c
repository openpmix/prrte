/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
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

#include "src/mca/base/base.h"

#include "src/util/proc_info.h"

#include "iof_prted.h"

/*
 * Local functions
 */
static int prrte_iof_prted_open(void);
static int prrte_iof_prted_close(void);
static int prrte_iof_prted_query(prrte_mca_base_module_t **module, int *priority);


/*
 * Public string showing the iof prted component version number
 */
const char *prrte_iof_prted_component_version_string =
"PRRTE prted iof MCA component version " PRRTE_VERSION;


prrte_iof_prted_component_t prrte_iof_prted_component = {
    {
        .iof_version = {
            PRRTE_IOF_BASE_VERSION_2_0_0,

            .mca_component_name = "prted",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),

            /* Component open, close, and query functions */
            .mca_open_component = prrte_iof_prted_open,
            .mca_close_component = prrte_iof_prted_close,
            .mca_query_component = prrte_iof_prted_query,
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
static int prrte_iof_prted_open(void)
{
    /* Nothing to do */
    return PRRTE_SUCCESS;
}

static int prrte_iof_prted_close(void)
{
    return PRRTE_SUCCESS;
}


static int prrte_iof_prted_query(prrte_mca_base_module_t **module, int *priority)
{
    /* if we are not a daemon, then don't use this module */
    if (!PRRTE_PROC_IS_DAEMON) {
        *module = NULL;
        *priority = -1;
        return PRRTE_ERROR;
    }

    *priority = 80;
    *module = (prrte_mca_base_module_t *) &prrte_iof_prted_module;

    return PRRTE_SUCCESS;
}

