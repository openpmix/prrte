/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2015 Research Organization for Information Science
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
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#include "src/mca/mca.h"
#include "src/mca/base/base.h"

#include "src/util/prrte_environ.h"
#include "src/util/output.h"

#include "src/util/show_help.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"

#include "src/mca/errmgr/base/static-components.h"

/*
 * Globals
 */
prrte_errmgr_base_t prrte_errmgr_base = {{{0}}};

/* Public module provides a wrapper around previous functions */
prrte_errmgr_base_module_t prrte_errmgr_default_fns = {
    .init = NULL, /* init     */
    .finalize = NULL, /* finalize */
    .logfn = prrte_errmgr_base_log,
    .abort = prrte_errmgr_base_abort,
    .abort_peers = prrte_errmgr_base_abort_peers
};
/* NOTE: ABSOLUTELY MUST initialize this
 * struct to include the log function as it
 * gets called even if the errmgr hasn't been
 * opened yet due to error
 */
prrte_errmgr_base_module_t prrte_errmgr = {
    .logfn = prrte_errmgr_base_log
};

static int prrte_errmgr_base_close(void)
{
    /* Close selected component */
    if (NULL != prrte_errmgr.finalize) {
        prrte_errmgr.finalize();
    }

    /* always leave a default set of fn pointers */
    prrte_errmgr = prrte_errmgr_default_fns;

    /* destruct the callback list */
    PRRTE_LIST_DESTRUCT(&prrte_errmgr_base.error_cbacks);

    return prrte_mca_base_framework_components_close(&prrte_errmgr_base_framework, NULL);
}

/**
 *  * Function for finding and opening either all MCA components, or the one
 *   * that was specifically requested via a MCA parameter.
 *    */
static int prrte_errmgr_base_open(prrte_mca_base_open_flag_t flags)
{
    /* load the default fns */
    prrte_errmgr = prrte_errmgr_default_fns;

    /* initialize the error callback list */
    PRRTE_CONSTRUCT(&prrte_errmgr_base.error_cbacks, prrte_list_t);

    /* Open up all available components */
    return prrte_mca_base_framework_components_open(&prrte_errmgr_base_framework, flags);
}

PRRTE_MCA_BASE_FRAMEWORK_DECLARE(prrte, errmgr, "PRRTE Error Manager", NULL,
                                 prrte_errmgr_base_open, prrte_errmgr_base_close,
                                 mca_errmgr_base_static_components, 0);
