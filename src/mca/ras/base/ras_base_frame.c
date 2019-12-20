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
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2015-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"

#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/event/event-internal.h"

#include "src/mca/ras/base/ras_private.h"
#include "src/mca/ras/base/base.h"


/* NOTE: the RAS does not require a proxy as only the
 * HNP can open the framework in prrte_init - non-HNP
 * procs are not allowed to allocate resources
 */

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * component's public prrte_mca_base_component_t struct.
 */

#include "src/mca/ras/base/static-components.h"

/*
 * Global variables
 */
prrte_ras_base_t prrte_ras_base = {0};

static int ras_register(prrte_mca_base_register_flag_t flags)
{
    prrte_ras_base.multiplier = 1;
    prrte_mca_base_var_register("prrte", "ras", "base", "multiplier",
                                "Simulate a larger cluster by launching N daemons/node",
                                PRRTE_MCA_BASE_VAR_TYPE_INT,
                                NULL, 0, 0,
                                PRRTE_INFO_LVL_9,
                                PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_ras_base.multiplier);
#if SLURM_CRAY_ENV
    /*
     * If we are in a Cray-SLURM environment, then we cannot
     * launch procs local to the HNP. The problem
     * is the MPI processes launched on the head node (where the
     * PRRTE_PROC_IS_MASTER evalues to true) get launched by a daemon
     * (mpirun) which is not a child of a slurmd daemon.  This
     * means that any RDMA credentials obtained via the odls/alps
     * local launcher are incorrect. Test for this condition. If
     * found, then take steps to ensure we launch a daemon on
     * the same node as mpirun and that it gets used to fork
     * local procs instead of mpirun so they get the proper
     * credential */

    prrte_ras_base.launch_orted_on_hn = true;
#else
    prrte_ras_base.launch_orted_on_hn = false;
#endif

    prrte_mca_base_var_register("prrte", "ras", "base", "launch_orted_on_hn",
                                "Launch an prrte daemon on the head node",
                                PRRTE_MCA_BASE_VAR_TYPE_BOOL,
                                NULL, 0, 0,
                                PRRTE_INFO_LVL_9,
                                PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_ras_base.launch_orted_on_hn);
    return PRRTE_SUCCESS;
}

static int prrte_ras_base_close(void)
{
    /* Close selected component */
    if (NULL != prrte_ras_base.active_module) {
        prrte_ras_base.active_module->finalize();
    }

    return prrte_mca_base_framework_components_close(&prrte_ras_base_framework, NULL);
}

/**
 *  * Function for finding and opening either all MCA components, or the one
 *   * that was specifically requested via a MCA parameter.
 *    */
static int prrte_ras_base_open(prrte_mca_base_open_flag_t flags)
{
    /* set default flags */
    prrte_ras_base.active_module = NULL;
    prrte_ras_base.allocation_read = false;
    prrte_ras_base.total_slots_alloc = 0;

    /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_ras_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, ras, "PRRTE Resource Allocation Subsystem",
                                 ras_register, prrte_ras_base_open, prrte_ras_base_close,
                                 prrte_ras_base_static_components, 0);
