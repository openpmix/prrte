/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file:
 *
 */

#ifndef _PRRTE_CONTEXT_FNS_H_
#define _PRRTE_CONTEXT_FNS_H_

#include "prrte_config.h"

#include "src/runtime/prrte_globals.h"

BEGIN_C_DECLS

PRRTE_EXPORT int prrte_util_check_context_app(prrte_app_context_t *context,
                                              char **env);

PRRTE_EXPORT int prrte_util_check_context_cwd(prrte_app_context_t *context,
                                              bool want_chdir);

END_C_DECLS
#endif
