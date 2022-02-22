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
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRTE_MCA_ERRMGR_PRIVATE_H
#define PRTE_MCA_ERRMGR_PRIVATE_H

/*
 * includes
 */
#include "prte_config.h"
#include "constants.h"
#include "types.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include "src/mca/plm/plm_types.h"
#include "src/runtime/prte_globals.h"

#include "src/mca/errmgr/errmgr.h"

/*
 * Functions for use solely within the ERRMGR framework
 */
BEGIN_C_DECLS

/* define a struct to hold framework-global values */
typedef struct {
    pmix_list_t error_cbacks;
} prte_errmgr_base_t;

PRTE_EXPORT extern prte_errmgr_base_t prte_errmgr_base;

/* declare the base default module */
PRTE_EXPORT extern prte_errmgr_base_module_t prte_errmgr_default_fns;

/*
 * Base functions
 */
PRTE_EXPORT void prte_errmgr_base_log(int error_code, char *filename, int line);

PRTE_EXPORT void prte_errmgr_base_abort(int error_code, char *fmt, ...)
    __prte_attribute_format__(__printf__, 2, 3);
PRTE_EXPORT int prte_errmgr_base_abort_peers(pmix_proc_t *procs, int32_t num_procs, int error_code);

END_C_DECLS
#endif
