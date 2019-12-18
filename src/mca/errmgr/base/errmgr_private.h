/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2010 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2011      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRRTE_MCA_ERRMGR_PRIVATE_H
#define PRRTE_MCA_ERRMGR_PRIVATE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif  /* HAVE_UNISTD_H */

#include "src/dss/dss_types.h"
#include "src/mca/plm/plm_types.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/errmgr/errmgr.h"

/*
 * Functions for use solely within the ERRMGR framework
 */
BEGIN_C_DECLS

/* define a struct to hold framework-global values */
typedef struct {
    prrte_list_t error_cbacks;
} prrte_errmgr_base_t;

PRRTE_EXPORT extern prrte_errmgr_base_t prrte_errmgr_base;

/* declare the base default module */
PRRTE_EXPORT extern prrte_errmgr_base_module_t prrte_errmgr_default_fns;

/*
 * Base functions
 */
PRRTE_EXPORT void prrte_errmgr_base_log(int error_code, char *filename, int line);

PRRTE_EXPORT void prrte_errmgr_base_abort(int error_code, char *fmt, ...)
    __prrte_attribute_format__(__printf__, 2, 3);
PRRTE_EXPORT int prrte_errmgr_base_abort_peers(prrte_process_name_t *procs,
                                               prrte_std_cntr_t num_procs,
                                               int error_code);

END_C_DECLS
#endif
