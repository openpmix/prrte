/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"

#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/plm/plm.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/base/base.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * module's public prrte_mca_base_module_t struct.
 */

#include "src/mca/plm/base/static-components.h"

/*
 * Global variables for use within PLM frameworks
 */
prrte_plm_globals_t prrte_plm_globals = {0};

/*
 * The default module
 */
prrte_plm_base_module_t prrte_plm = {0};


static int mca_plm_base_register(prrte_mca_base_register_flag_t flags)
{
    prrte_plm_globals.node_regex_threshold = 1024;
    (void) prrte_mca_base_framework_var_register (&prrte_plm_base_framework, "node_regex_threshold",
                                                  "Only pass the node regex on the orted command line if smaller than this threshold",
                                                  PRRTE_MCA_BASE_VAR_TYPE_SIZE_T, NULL, 0,
                                                  PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                                  PRRTE_INFO_LVL_9,
                                                  PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                  &prrte_plm_globals.node_regex_threshold);
    return PRRTE_SUCCESS;
}

static int prrte_plm_base_close(void)
{
    int rc;

    /* Close the selected component */
    if( NULL != prrte_plm.finalize ) {
        prrte_plm.finalize();
    }

   /* if we are the HNP, then stop our receive */
    if (PRRTE_PROC_IS_MASTER) {
        if (PRRTE_SUCCESS != (rc = prrte_plm_base_comm_stop())) {
            PRRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    return prrte_mca_base_framework_components_close(&prrte_plm_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components,
 * or the one that was specifically requested via a MCA parameter.
 */
static int prrte_plm_base_open(prrte_mca_base_open_flag_t flags)
{
    /* init the next jobid */
    prrte_plm_globals.next_jobid = 1;

    /* default to assigning daemons to nodes at launch */
    prrte_plm_globals.daemon_nodes_assigned_at_launch = true;

     /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_plm_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, plm, NULL, mca_plm_base_register,
                                 prrte_plm_base_open, prrte_plm_base_close,
                                 prrte_plm_base_static_components, 0);
