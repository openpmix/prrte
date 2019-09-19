/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file **/

#include "prrte_config.h"
#include "constants.h"

#include "src/util/output.h"
#include "src/util/argv.h"
#include "src/mca/base/prrte_mca_base_framework.h"

#include "src/mca/ess/ess.h"
#include "src/mca/ess/base/base.h"
#include "src/mca/schizo/base/base.h"
#include "src/runtime/prrte_globals.h"
#include "src/runtime/prrte_locks.h"
#include "src/runtime/runtime.h"
#include "src/util/listener.h"
#include "src/util/name_fns.h"
#include "src/util/proc_info.h"
#include "src/util/show_help.h"

int prrte_finalize(void)
{
    int rc;

    --prrte_initialized;
    if (0 != prrte_initialized) {
        /* check for mismatched calls */
        if (0 > prrte_initialized) {
            prrte_output(0, "%s MISMATCHED CALLS TO PRRTE FINALIZE",
                        PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        }
        return PRRTE_ERROR;
    }

    /* protect against multiple calls */
    if (prrte_atomic_trylock(&prrte_finalize_lock)) {
        return PRRTE_SUCCESS;
    }

    /* flag that we are finalizing */
    prrte_finalizing = true;

    /* stop listening for connections - will
     * be ignored if no listeners were registered */
    prrte_stop_listening();

    /* call the finalize function for this environment */
    if (PRRTE_SUCCESS != (rc = prrte_ess.finalize())) {
        return rc;
    }

    /* finalize schizo */
    prrte_schizo.finalize();

    /* Close the general debug stream */
    prrte_output_close(prrte_debug_output);

    if (NULL != prrte_fork_agent) {
        prrte_argv_free(prrte_fork_agent);
    }

    /* finalize the class/object system */
    prrte_class_finalize();

    free (prrte_process_info.nodename);
    prrte_process_info.nodename = NULL;

    return PRRTE_SUCCESS;

    return rc;
}
