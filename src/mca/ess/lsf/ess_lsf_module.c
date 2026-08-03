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
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016-2019 Research Organization for Information Science
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

#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/pmix/pmix-internal.h"
#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/ess/base/base.h"
#include "src/mca/ess/ess.h"
#include "src/mca/ess/lsf/ess_lsf.h"

static int lsf_set_name(void);

static int rte_init(int argc, char **argv);
static int rte_finalize(void);

prte_ess_base_module_t prte_ess_lsf_module = {
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
    if (PRTE_SUCCESS != (ret = lsf_set_name())) {
        error = "lsf_set_name";
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

static int lsf_set_name(void)
{
    PMIX_OUTPUT_VERBOSE((1, prte_ess_base_framework.framework_output, "ess:lsf setting name"));

    /* LSF gives every daemon it starts the same base vpid; each one adds its
     * own task index to arrive at a unique rank.  LSF numbers those tasks
     * from one, so back the index off by one to land on the same zero-based
     * rank every other environment produces. */
    return prte_ess_base_set_identity("LSF_PM_TASKID", -1);
}
