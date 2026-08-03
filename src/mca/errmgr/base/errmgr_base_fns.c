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
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>
#if HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */
#if HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#if HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif /* HAVE_SYS_STAT_H */
#ifdef HAVE_DIRENT_H
#    include <dirent.h>
#endif /* HAVE_DIRENT_H */
#include <time.h>

#include <stdarg.h>
#include <stdlib.h>

#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"

#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/session_dir.h"

#include "src/runtime/prte_globals.h"
#include "src/runtime/prte_locks.h"
#include "src/runtime/prte_wait.h"
#include "src/runtime/runtime.h"

#include "src/mca/ess/ess.h"
#include "src/mca/odls/odls.h"
#include "src/mca/plm/plm.h"
#include "src/mca/state/state.h"

#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"
#include "src/mca/errmgr/errmgr.h"

/*
 * Public interfaces
 */
void prte_errmgr_base_log(int error_code, char *filename, int line)
{
    char *errstring = NULL;

    errstring = (char *) PRTE_ERROR_NAME(error_code);

    if (NULL == errstring) {
        /* if the error is silent, say nothing */
        return;
    }

    pmix_output(0, "%s PRTE_ERROR_LOG: %s in file %s at line %d",
                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), errstring, filename, line);
}

bool prte_errmgr_base_any_live_children(const char *job)
{
    int i;
    prte_proc_t *child;

    if (NULL == prte_local_children) {
        return false;
    }

    for (i = 0; i < prte_local_children->size; i++) {
        child = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i);
        if (NULL == child) {
            continue;
        }
        /* is this child part of the specified job? */
        if ((PMIX_NSPACE_INVALID(job) || PMIX_CHECK_NSPACE(job, child->name.nspace))
            && PRTE_FLAG_TEST(child, PRTE_PROC_FLAG_ALIVE)) {
            PMIX_OUTPUT_VERBOSE((5, prte_errmgr_base_framework.framework_output,
                                 "%s errmgr: proc %s is still alive",
                                 PRTE_NAME_PRINT(PRTE_PROC_MY_NAME),
                                 PRTE_NAME_PRINT(&child->name)));
            return true;
        }
    }

    /* if we get here, then nobody is left alive from that job */
    return false;
}

int prte_errmgr_base_pack_state_for_proc(pmix_data_buffer_t *alert, prte_proc_t *child)
{
    int rc;

    /* pack the child's vpid */
    rc = PMIx_Data_pack(NULL, alert, &child->name.rank, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pack the pid */
    rc = PMIx_Data_pack(NULL, alert, &child->pid, 1, PMIX_PID);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pack its state */
    rc = PMIx_Data_pack(NULL, alert, &child->state, 1, PMIX_UINT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    /* pack its exit code */
    rc = PMIx_Data_pack(NULL, alert, &child->exit_code, 1, PMIX_INT32);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    return PRTE_SUCCESS;
}

int prte_errmgr_base_pack_state_update(pmix_data_buffer_t *alert, prte_job_t *jobdat)
{
    int rc, i;
    prte_proc_t *child;
    pmix_rank_t null = PMIX_RANK_INVALID;

    /* pack the jobid */
    rc = PMIx_Data_pack(NULL, alert, &jobdat->nspace, 1, PMIX_PROC_NSPACE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }
    if (NULL != prte_local_children) {
        for (i = 0; i < prte_local_children->size; i++) {
            child = (prte_proc_t *) pmix_pointer_array_get_item(prte_local_children, i);
            if (NULL == child) {
                continue;
            }
            /* if this child is part of the job... */
            if (PMIX_CHECK_NSPACE(child->name.nspace, jobdat->nspace)) {
                rc = prte_errmgr_base_pack_state_for_proc(alert, child);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    return rc;
                }
            }
        }
    }
    /* flag that this job is complete so the receiver can know */
    rc = PMIx_Data_pack(NULL, alert, &null, 1, PMIX_PROC_RANK);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    return PRTE_SUCCESS;
}
