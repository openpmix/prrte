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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"
#include "src/mca/base/prte_mca_base_alias.h"
#include "src/mca/mca.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"

/*
 * The following file was created by configure.  It contains extern
 * statements and the definition of an array of pointers to each
 * module's public prte_mca_base_module_t struct.
 */

#include "src/mca/plm/base/static-components.h"

/*
 * Global variables for use within PLM frameworks
 */
prte_plm_globals_t prte_plm_globals = {0};

/*
 * The default module
 */
prte_plm_base_module_t prte_plm = {0};

static int mca_plm_base_register(prte_mca_base_register_flag_t flags)
{
    prte_plm_globals.node_regex_threshold = 1024;
    (void) prte_mca_base_framework_var_register(
        &prte_plm_base_framework, "node_regex_threshold",
        "Only pass the node regex on the orted command line if smaller than this threshold",
        PRTE_MCA_BASE_VAR_TYPE_SIZE_T, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_INTERNAL, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_plm_globals.node_regex_threshold);

    /* Note that we break abstraction rules here by listing a
     specific PLM here in the base.  This is necessary, however,
     due to extraordinary circumstances:

     1. In PRRTE v2.0, we want to rename the "rsh" PLM to be
        "ssh" to more closely represent its usage.

     2. The MCA aliasing mechanism was therefore ported from
        OMPI for this purpose. Both the component itself and
        all of its MCA vars are aliased.

     3. However -- at least as currently implemented -- by the time
     individual components are registered, it's too late to make
     aliases.  Hence, if we want to preserve the name "rsh" for
     some sembalance of backwards compatibility (and we do!), we
     have to register "rsh" as an "alias for ssh" up here in
     the PLM base, before any PLM components are registered.

     This is why we tolerate this abstraction break up here in the
     PLM component base. */
    (void) prte_mca_base_alias_register("prte", "plm", "ssh", "rsh", PRTE_MCA_BASE_ALIAS_FLAG_NONE);
    return PRTE_SUCCESS;
}

static int prte_plm_base_close(void)
{
    int rc;

    /* Close the selected component */
    if (NULL != prte_plm.finalize) {
        prte_plm.finalize();
    }

    /* if we are the HNP, then stop our receive */
    if (PRTE_PROC_IS_MASTER) {
        if (PRTE_SUCCESS != (rc = prte_plm_base_comm_stop())) {
            PRTE_ERROR_LOG(rc);
            return rc;
        }
    }

    if (NULL != prte_plm_globals.base_nspace) {
        free(prte_plm_globals.base_nspace);
    }

    return prte_mca_base_framework_components_close(&prte_plm_base_framework, NULL);
}

/**
 * Function for finding and opening either all MCA components,
 * or the one that was specifically requested via a MCA parameter.
 */
static int prte_plm_base_open(prte_mca_base_open_flag_t flags)
{
    /* init the next jobid */
    prte_plm_globals.next_jobid = 1;

    /* default to assigning daemons to nodes at launch */
    prte_plm_globals.daemon_nodes_assigned_at_launch = true;

    /* Open up all available components */
    return prte_mca_base_framework_components_open(&prte_plm_base_framework, flags);
}

PRTE_MCA_BASE_FRAMEWORK_DECLARE(prte, plm, NULL, mca_plm_base_register, prte_plm_base_open,
                                prte_plm_base_close, prte_plm_base_static_components,
                                PRTE_MCA_BASE_FRAMEWORK_FLAG_DEFAULT);
