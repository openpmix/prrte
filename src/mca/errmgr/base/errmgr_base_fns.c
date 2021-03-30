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
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
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

#include "src/mca/base/base.h"
#include "src/mca/mca.h"
#include "src/util/argv.h"
#include "src/util/basename.h"
#include "src/util/os_dirpath.h"
#include "src/util/output.h"
#include "src/util/printf.h"

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
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/routed.h"
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

    prte_output(0, "%s PRTE_ERROR_LOG: %s in file %s at line %d",
                PRTE_NAME_PRINT(PRTE_PROC_MY_NAME), errstring, filename, line);
}

void prte_errmgr_base_abort(int error_code, char *fmt, ...)
{
    va_list arglist;

    /* If there was a message, output it */
    va_start(arglist, fmt);
    if (NULL != fmt) {
        char *buffer = NULL;
        prte_vasprintf(&buffer, fmt, arglist);
        prte_output(0, "%s", buffer);
        free(buffer);
    }
    va_end(arglist);

    /* if I am a daemon or the HNP... */
    if (PRTE_PROC_IS_MASTER || PRTE_PROC_IS_DAEMON) {
        /* whack my local procs */
        if (NULL != prte_odls.kill_local_procs) {
            prte_odls.kill_local_procs(NULL);
        }
        /* whack any session directories */
        prte_session_dir_cleanup(PRTE_JOBID_WILDCARD);
    }

    /* if a critical connection failed, or a sensor limit was exceeded, exit without dropping a core
     */
    if (PRTE_ERR_CONNECTION_FAILED == error_code || PRTE_ERR_SENSOR_LIMIT_EXCEEDED == error_code) {
        prte_ess.abort(error_code, false);
    } else {
        prte_ess.abort(error_code, true);
    }

    /*
     * We must exit in prte_ess.abort; all implementations of prte_ess.abort
     * contain __prte_attribute_noreturn__
     */
    /* No way to reach here */
}

int prte_errmgr_base_abort_peers(pmix_proc_t *procs, int32_t num_procs, int error_code)
{
    return PRTE_ERR_NOT_IMPLEMENTED;
}
