/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
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

#include <stdio.h>
#include <sys/types.h>
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <stdlib.h>

#include "src/event/event-internal.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"
#include "src/util/prte_show_help.h"

#include "src/mca/errmgr/base/base.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/ess/ess.h"
#include "src/grpcomm/grpcomm.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/plm/base/base.h"
#include "src/rml/rml.h"

#include "src/mca/filem/base/base.h"
#include "src/mca/rmaps/base/base.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"

#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_wait.h"
#include "src/runtime/runtime.h"

#include "src/mca/ess/base/base.h"
#include "src/mca/ess/env/ess_env.h"
#include "src/mca/ess/ess.h"

static int env_set_name(void);

static int rte_init(int argc, char **argv);
static int rte_finalize(void);

prte_ess_base_module_t prte_ess_env_module = {
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

    /* Start by getting a unique name from the enviro */
    if (PRTE_SUCCESS != (ret = env_set_name())) {
        error = "env_set_name";
        goto error;
    }

    /* if I am a daemon, complete my setup using the
     * default procedure
     */
    if (PRTE_SUCCESS != (ret = prte_ess_base_prted_setup())) {
        PRTE_ERROR_LOG(ret);
        error = "prte_ess_base_prted_setup";
        goto error;
    }
    return PRTE_SUCCESS;

error:
    if (PRTE_ERR_SILENT != ret && !prte_report_silent_errors) {
        prte_show_help("help-prte-runtime.txt", "prte_init:startup:internal-failure", true, error,
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

static int env_set_name(void)
{
    /* an ssh launch assigns each daemon its vpid directly, so there is no
     * per-node index to add */
    return prte_ess_base_set_identity(NULL, 0);
}
