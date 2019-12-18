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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <errno.h>

#include "src/util/prrte_environ.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/util/proc_info.h"

#include "src/mca/ess/base/base.h"

int prrte_ess_env_get(void)
{
    if (prrte_ess_base_num_procs < 0) {
        PRRTE_ERROR_LOG(PRRTE_ERR_NOT_FOUND);
        return PRRTE_ERR_NOT_FOUND;
    }
    prrte_process_info.num_procs = (prrte_std_cntr_t)prrte_ess_base_num_procs;

    if (prrte_process_info.max_procs < prrte_process_info.num_procs) {
        prrte_process_info.max_procs = prrte_process_info.num_procs;
    }

    return PRRTE_SUCCESS;
}
