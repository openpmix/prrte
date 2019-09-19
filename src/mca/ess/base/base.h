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
 * Copyright (c) 2011-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012      Oak Ridge National Labs.  All rights reserved.
 * Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
 * Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef MCA_ESS_BASE_H
#define MCA_ESS_BASE_H

#include "prrte_config.h"
#include "types.h"

#include "src/mca/base/prrte_mca_base_framework.h"
#include "src/mca/mca.h"
#include "src/dss/dss_types.h"
#include "src/pmix/pmix-internal.h"

#include "src/mca/ess/ess.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_ess_base_framework;
/**
 * Select a ess module
 */
PRRTE_EXPORT int prrte_ess_base_select(void);

/*
 * stdout/stderr buffering control parameter
 */
PRRTE_EXPORT extern int prrte_ess_base_std_buffering;

PRRTE_EXPORT extern int prrte_ess_base_num_procs;
PRRTE_EXPORT extern char *prrte_ess_base_jobid;
PRRTE_EXPORT extern char *prrte_ess_base_vpid;
PRRTE_EXPORT extern prrte_list_t prrte_ess_base_signals;

/*
 * Internal helper functions used by components
 */
PRRTE_EXPORT int prrte_ess_env_get(void);

PRRTE_EXPORT int prrte_ess_base_std_prolog(void);

PRRTE_EXPORT int prrte_ess_base_prted_setup(void);
PRRTE_EXPORT int prrte_ess_base_prted_finalize(void);

/* Detect whether or not this proc is bound - if not,
 * see if it should bind itself
 */
PRRTE_EXPORT int prrte_ess_base_proc_binding(void);

/*
 * Put functions
 */
PRRTE_EXPORT int prrte_ess_env_put(prrte_std_cntr_t num_procs,
                                   prrte_std_cntr_t num_local_procs,
                                   char ***env);

typedef struct {
    prrte_list_item_t super;
    char *signame;
    int signal;
} prrte_ess_base_signal_t;
PRRTE_CLASS_DECLARATION(prrte_ess_base_signal_t);

END_C_DECLS

#endif
