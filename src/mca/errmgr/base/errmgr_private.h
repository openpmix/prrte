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
 * Copyright (c) 2021-2024 Nanook Consulting.  All rights reserved.
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

/* declare the base default module */
PRTE_EXPORT extern prte_errmgr_base_module_t prte_errmgr_default_fns;

/*
 * Base functions
 */
PRTE_EXPORT void prte_errmgr_base_log(int error_code, char *filename, int line);

/* Is any local child still alive?
 *
 * Every errmgr handler that has to decide "is this node empty yet" asks this
 * question, and each one used to spell out its own scan of
 * prte_local_children.  They must not: the loop variable of such a scan sits
 * in the same scope as the proc whose error is being handled, and clobbering
 * the latter makes the handler report the wrong proc (that bug was live in
 * errmgr/prted).  Ask here instead.
 *
 * Pass an invalid nspace to ask about any job, or a job's nspace to restrict
 * the question to that job's children.
 */
PRTE_EXPORT bool prte_errmgr_base_any_live_children(const char *job);

END_C_DECLS
#endif
