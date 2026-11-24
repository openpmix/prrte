/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2022 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2018-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"

#include "plm_tm.h"
#include "src/mca/plm/base/base.h"
#include "src/mca/plm/base/plm_private.h"
#include "src/mca/plm/plm.h"

/*
 * Public string showing the plm tm component version number
 */
const char *prte_mca_plm_tm_component_version_string
    = "PRTE tm plm MCA component version " PRTE_VERSION;

static int plm_tm_open(void);
static int plm_tm_close(void);
static int prte_mca_plm_tm_component_query(pmix_mca_base_module_t **module, int *priority);

prte_mca_plm_tm_component_t prte_mca_plm_tm_component = {
    .super = {
        PRTE_MCA_BASE_VERSION(plm),

        /* Component name and version */
        .pmix_mca_component_name = "tm",
        PMIX_MCA_BASE_MAKE_VERSION(component,
                                   PRTE_MAJOR_VERSION,
                                   PRTE_MINOR_VERSION,
                                   PRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .pmix_mca_open_component = plm_tm_open,
        .pmix_mca_close_component = plm_tm_close,
        .pmix_mca_query_component = prte_mca_plm_tm_component_query,
    }
};
PMIX_MCA_BASE_COMPONENT_INIT(prte, plm, tm)

static int plm_tm_open(void)
{
    prte_mca_plm_tm_component.helper = NULL;
    return PRTE_SUCCESS;
}

static int plm_tm_close(void)
{
    free(prte_mca_plm_tm_component.helper);
    prte_mca_plm_tm_component.helper = NULL;
    return PRTE_SUCCESS;
}

static int prte_mca_plm_tm_component_query(pmix_mca_base_module_t **module, int *priority)
{
    char *helper;

    *module = NULL;

    /* Are we running under a PBS job? */
    if (NULL == getenv("PBS_ENVIRONMENT") || NULL == getenv("PBS_JOBID")) {
        return PRTE_ERROR;
    }

    /* We can launch only through the helper, which is installed with
     * us. Without it, say so: whatever launches instead starts daemons
     * that PBS does not account for. */
    helper = pmix_os_path(false, prte_install_dirs.libexecdir, PRTE_PLM_TM_HELPER_DIR,
                          PRTE_PLM_TM_HELPER_NAME, NULL);
    if (NULL == helper) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    if (0 != access(helper, X_OK)) {
        pmix_show_help("help-plm-tm.txt", "no-helper", true, helper, strerror(errno));
        free(helper);
        return PRTE_ERROR;
    }
    free(prte_mca_plm_tm_component.helper);
    prte_mca_plm_tm_component.helper = helper;

    *priority = 75;
    *module = (pmix_mca_base_module_t *) &prte_plm_tm_module;
    return PRTE_SUCCESS;
}
