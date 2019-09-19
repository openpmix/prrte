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
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012      Los Alamos National Security, LLC. All rights reserved
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/** @file:
 *
 */

#ifndef _PRRTE_ERROR_STRINGS_H_
#define _PRRTE_ERROR_STRINGS_H_

#include "prrte_config.h"

#include "src/runtime/prrte_globals.h"
#include "src/mca/plm/plm_types.h"

BEGIN_C_DECLS

PRRTE_EXPORT const char *prrte_job_state_to_str(prrte_job_state_t state);

PRRTE_EXPORT const char *prrte_app_ctx_state_to_str(prrte_app_state_t state);

PRRTE_EXPORT const char *prrte_proc_state_to_str(prrte_proc_state_t state);

PRRTE_EXPORT const char *prrte_node_state_to_str(prrte_node_state_t state);

END_C_DECLS
#endif
