/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prte_config.h"
#include "constants.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <ctype.h>
#include <string.h>

#include "src/class/pmix_pointer_array.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/rml/rml.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/ess/base/base.h"
#include "src/mca/ess/ess.h"
#include "src/mca/ess/slurm/ess_slurm.h"

static int slurm_set_name(void);

static int rte_init(int argc, char **argv);
static int rte_finalize(void);

prte_ess_base_module_t prte_ess_slurm_module = {
    .init = rte_init,
    .finalize = rte_finalize
};

static int rte_init(int argc, char **argv)
{
    int ret;
    char *error = NULL;
    PRTE_HIDE_UNUSED_PARAMS(argc, argv);

    /* run the prolog */
    if (PRTE_SUCCESS != (ret = prte_ess_base_std_prolog())) {
        error = "prte_ess_base_std_prolog";
        goto error;
    }

    /* Start by getting a unique name */
    if (PRTE_SUCCESS != (ret = slurm_set_name())) {
        error = "slurm_set_name";
        goto error;
    }

    if (PRTE_SUCCESS != (ret = prte_ess_base_prted_setup())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_ess_base_prted_setup";
        goto error;
    }
    return PRTE_SUCCESS;

error:
    if (PRTE_ERR_SILENT != ret && !prte_report_silent_errors) {
        pmix_show_help("help-prte-runtime.txt", "prte_init:startup:internal-failure", true, error,
                       PRTE_ERROR_NAME(ret), ret);
    }

    return ret;
}

static int rte_finalize(void)
{
    int ret;

    if (PRTE_SUCCESS != (ret = prte_ess_base_prted_finalize())) {
        PRTE_ERROR_LOG(ret);
    }

    return ret;
}

static int slurm_set_name(void)
{
    char *tmp;
    int rc;

    PMIX_OUTPUT_VERBOSE((1, prte_ess_base_framework.framework_output, "ess:slurm setting name"));

    /* SLURM gives every daemon it starts the same base vpid; each one adds
     * its own index within the allocation to arrive at a unique rank */
    rc = prte_ess_base_set_identity("SLURM_NODEID", 0);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    /* fix up the system info nodename to match exactly what slurm returned.
     * Read the replacement BEFORE discarding what we have: releasing it first
     * and then failing would leave prte_process_info.nodename dangling for
     * every later reader - including the error path we are about to take. */
    if (NULL == (tmp = getenv("SLURMD_NODENAME"))) {
        PRTE_ERROR_LOG(PRTE_ERR_NOT_FOUND);
        return PRTE_ERR_NOT_FOUND;
    }
    if (NULL != prte_process_info.nodename) {
        free(prte_process_info.nodename);
    }
    prte_process_info.nodename = strdup(tmp);

    PMIX_OUTPUT_VERBOSE(
        (1, prte_ess_base_framework.framework_output, "ess:slurm set nodename to %s",
         (NULL == prte_process_info.nodename) ? "NULL" : prte_process_info.nodename));

    return PRTE_SUCCESS;
}
