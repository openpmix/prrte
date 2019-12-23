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
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
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

#include "src/util/show_help.h"
#include "src/util/argv.h"

#include "src/util/proc_info.h"
#include "src/mca/errmgr/base/base.h"
#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/ess/ess.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/ess/alps/ess_alps.h"

#include <errno.h>

static int alps_set_name(void);
static int rte_init(int argc, char **argv);
static int rte_finalize(void);

prrte_ess_base_module_t prrte_ess_alps_module = {
    rte_init,
    rte_finalize,
    NULL,
    NULL /* ft_event */
};

/* Local variables */
static prrte_vpid_t starting_vpid = 0;


static int rte_init(int argc, char **argv)
{
    int ret;
    char *error = NULL;

    PRRTE_OUTPUT_VERBOSE((1, prrte_ess_base_framework.framework_output,
                         "ess:alps in rte_init"));

    /* run the prolog */
    if (PRRTE_SUCCESS != (ret = prrte_ess_base_std_prolog())) {
        error = "prrte_ess_base_std_prolog";
        goto fn_fail;
    }

    if (PRRTE_SUCCESS != (ret = alps_set_name())) {
        error = "alps_set_name";
        goto fn_fail;
    }

    if (PRRTE_SUCCESS != (ret = prrte_ess_base_prted_setup())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_ess_base_prted_setup";
        goto fn_fail;
    }

    /*
     * now synchronize with aprun.
     */

    if (PRRTE_SUCCESS != (ret = prrte_ess_alps_sync_start())) {
        error = "prrte_ess_alps_sync";
        goto fn_fail;
    }

    return PRRTE_SUCCESS;

   fn_fail:
    if (PRRTE_ERR_SILENT != ret && !prrte_report_silent_errors) {
        prrte_show_help("help-prrte-runtime.txt",
                       "prrte_init:startup:internal-failure",
                       true, error, PRRTE_ERROR_NAME(ret), ret);
    }
    return ret;
}

static int rte_finalize(void)
{
    int ret = PRRTE_SUCCESS;

    if (PRRTE_SUCCESS != (ret = prrte_ess_base_prted_finalize())) {
        PRRTE_ERROR_LOG(ret);
        goto fn_exit;
    }

    /* notify alps that we're done */
    if (PRRTE_SUCCESS != (ret = prrte_ess_alps_sync_complete())) {
        PRRTE_ERROR_LOG(ret);
    }

   fn_exit:
    return ret;
}

static int alps_set_name(void)
{
    int rc;
    int rank;
    prrte_jobid_t jobid;

    if (NULL == prrte_ess_base_jobid) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }
    if (PRRTE_SUCCESS != (rc = prrte_util_convert_string_to_jobid(&jobid, prrte_ess_base_jobid))) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    if (NULL == prrte_ess_base_vpid) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }
    if (PRRTE_SUCCESS != (rc = prrte_util_convert_string_to_vpid(&starting_vpid,
                                                               prrte_ess_base_vpid))) {
        PRRTE_ERROR_LOG(rc);
        return(rc);
    }

    PRRTE_PROC_MY_NAME->jobid = jobid;

    if (PRRTE_SUCCESS != (rc = prrte_ess_alps_get_first_rank_on_node(&rank))) {
        PRRTE_ERROR_LOG(rc);
        return(rc);
    }

    PRRTE_PROC_MY_NAME->vpid = (prrte_vpid_t)rank + starting_vpid;

    /* get the num procs as provided in the cmd line param */
    if (PRRTE_SUCCESS != (rc = prrte_ess_env_get())) {
        PRRTE_ERROR_LOG(rc);
        return rc;
    }

    return PRRTE_SUCCESS;
}
