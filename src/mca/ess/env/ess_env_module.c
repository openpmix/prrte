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
 * Copyright (c) 2011-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#include "prrte_config.h"
#include "constants.h"

#include <sys/types.h>
#include <stdio.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>

#include "src/event/event-internal.h"

#include "src/util/show_help.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/util/output.h"
#include "src/util/malloc.h"
#include "src/util/argv.h"

#include "src/mca/rml/base/base.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/base/base.h"
#include "src/mca/routed/routed.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/grpcomm/base/base.h"
#include "src/mca/iof/base/base.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/ess/ess.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/plm/base/base.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/filem/base/base.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"
#include "src/util/name_fns.h"

#include "src/runtime/runtime.h"
#include "src/runtime/prrte_wait.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/ess/ess.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/ess/env/ess_env.h"

static int env_set_name(void);

static int rte_init(int argc, char **argv);
static int rte_finalize(void);

prrte_ess_base_module_t prrte_ess_env_module = {
    rte_init,
    rte_finalize,
    NULL,
    NULL
};

static int rte_init(int argc, char **argv)
{
    int ret;
    char *error = NULL;

    /* run the prolog */
    if (PRRTE_SUCCESS != (ret = prrte_ess_base_std_prolog())) {
        error = "prrte_ess_base_std_prolog";
        goto error;
    }

    /* Start by getting a unique name from the enviro */
    env_set_name();

    /* if I am a daemon, complete my setup using the
     * default procedure
     */
    if (PRRTE_SUCCESS != (ret = prrte_ess_base_prted_setup())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_ess_base_prted_setup";
        goto error;
    }
    return PRRTE_SUCCESS;

 error:
    if (PRRTE_ERR_SILENT != ret && !prrte_report_silent_errors) {
        prrte_show_help("help-prrte-runtime.txt",
                       "prrte_init:startup:internal-failure",
                       true, error, PRRTE_ERROR_NAME(ret), ret);
    }

    return ret;
}

static int rte_finalize(void)
{
    int ret;

    if (PRRTE_SUCCESS != (ret = prrte_ess_base_prted_finalize())) {
        PRRTE_ERROR_LOG(ret);
    }
    return ret;
}

static int env_set_name(void)
{
    int rc;
    prrte_jobid_t jobid;
    prrte_vpid_t vpid;

    if (NULL == prrte_ess_base_jobid) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }
    if (PRRTE_SUCCESS != (rc = prrte_util_convert_string_to_jobid(&jobid, prrte_ess_base_jobid))) {
        PRRTE_ERROR_LOG(rc);
        return(rc);
    }

    if (NULL == prrte_ess_base_vpid) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }
    if (PRRTE_SUCCESS != (rc = prrte_util_convert_string_to_vpid(&vpid, prrte_ess_base_vpid))) {
        PRRTE_ERROR_LOG(rc);
        return(rc);
    }

    PRRTE_PROC_MY_NAME->jobid = jobid;
    PRRTE_PROC_MY_NAME->vpid = vpid;

    PRRTE_OUTPUT_VERBOSE((1, prrte_ess_base_framework.framework_output,
                         "ess:env set name to %s", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME)));

    /* get the non-name common environmental variables */
    if (PRRTE_SUCCESS != (rc = prrte_ess_env_get())) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    return PRRTE_SUCCESS;
}
