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
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */


#include "prrte_config.h"
#include "constants.h"

#include <string.h>
#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif  /* HAVE_SYS_TYPES_H */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */
#if HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif /* HAVE_SYS_TYPES_H */
#if HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif /* HAVE_SYS_STAT_H */
#ifdef HAVE_DIRENT_H
#include <dirent.h>
#endif /* HAVE_DIRENT_H */
#include <time.h>

#include <stdlib.h>
#include <stdarg.h>

#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/util/os_dirpath.h"
#include "src/util/output.h"
#include "src/util/printf.h"
#include "src/util/basename.h"
#include "src/util/argv.h"

#include "src/util/name_fns.h"
#include "src/util/session_dir.h"
#include "src/util/proc_info.h"

#include "src/runtime/prrte_globals.h"
#include "src/runtime/runtime.h"
#include "src/runtime/prrte_wait.h"
#include "src/runtime/prrte_locks.h"

#include "src/mca/ess/ess.h"
#include "src/mca/state/state.h"
#include "src/mca/odls/odls.h"
#include "src/mca/plm/plm.h"
#include "src/mca/rml/rml.h"
#include "src/mca/rml/rml_types.h"
#include "src/mca/routed/routed.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"

/*
 * Public interfaces
 */
void prrte_errmgr_base_log(int error_code, char *filename, int line)
{
    char *errstring = NULL;

    errstring = (char*)PRRTE_ERROR_NAME(error_code);

    if (NULL == errstring) {
        /* if the error is silent, say nothing */
        return;
    }

    prrte_output(0, "%s PRRTE_ERROR_LOG: %s in file %s at line %d",
                PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME),
                errstring, filename, line);
}

void prrte_errmgr_base_abort(int error_code, char *fmt, ...)
{
    va_list arglist;

    /* If there was a message, output it */
    va_start(arglist, fmt);
    if( NULL != fmt ) {
        char* buffer = NULL;
        prrte_vasprintf( &buffer, fmt, arglist );
        prrte_output( 0, "%s", buffer );
        free( buffer );
    }
    va_end(arglist);

    /* if I am a daemon or the HNP... */
    if (PRRTE_PROC_IS_MASTER || PRRTE_PROC_IS_DAEMON) {
        /* whack my local procs */
        prrte_odls.kill_local_procs(NULL);
        /* whack any session directories */
        prrte_session_dir_cleanup(PRRTE_JOBID_WILDCARD);
    }

    /* if a critical connection failed, or a sensor limit was exceeded, exit without dropping a core */
    if (PRRTE_ERR_CONNECTION_FAILED == error_code ||
        PRRTE_ERR_SENSOR_LIMIT_EXCEEDED == error_code) {
        prrte_ess.abort(error_code, false);
    } else {
        prrte_ess.abort(error_code, true);
    }

    /*
     * We must exit in prrte_ess.abort; all implementations of prrte_ess.abort
     * contain __prrte_attribute_noreturn__
     */
    /* No way to reach here */
}

int prrte_errmgr_base_abort_peers(prrte_process_name_t *procs,
                                 prrte_std_cntr_t num_procs,
                                 int error_code)
{
    return PRRTE_ERR_NOT_IMPLEMENTED;
}
