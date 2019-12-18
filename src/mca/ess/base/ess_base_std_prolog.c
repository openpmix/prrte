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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
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

#include "src/mca/errmgr/errmgr.h"
#include "src/util/show_help.h"
#include "src/runtime/prrte_wait.h"
#include "src/runtime/runtime_internals.h"

#include "src/mca/ess/base/base.h"

int prrte_ess_base_std_prolog(void)
{
    int ret;
    char *error = NULL;

    /* Initialize the PRRTE data type support */
    if (PRRTE_SUCCESS != (ret = prrte_dt_init())) {
        error = "prrte_dt_init";
        goto error;
    }
    /*
     * Setup the waitpid/sigchld system
     */
    if (PRRTE_SUCCESS != (ret = prrte_wait_init())) {
        PRRTE_ERROR_LOG(ret);
        error = "prrte_wait_init";
        goto error;
    }

    return PRRTE_SUCCESS;

 error:
    prrte_show_help("help-prrte-runtime",
                   "prrte_init:startup:internal-failure",
                   true, error, PRRTE_ERROR_NAME(ret), ret);

    return ret;
}
