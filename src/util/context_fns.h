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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file:
 *
 */

#ifndef _PRTE_CONTEXT_FNS_H_
#define _PRTE_CONTEXT_FNS_H_

#include "prte_config.h"

#include "src/runtime/prte_globals.h"

BEGIN_C_DECLS

PRTE_EXPORT int prte_util_check_context_app(prte_app_context_t *context, char **env);

PRTE_EXPORT int prte_util_check_context_cwd(prte_app_context_t *context, bool want_chdir);

END_C_DECLS
#endif
