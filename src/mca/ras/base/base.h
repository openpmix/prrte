/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRRTE_MCA_RAS_BASE_H
#define PRRTE_MCA_RAS_BASE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "src/mca/base/prrte_mca_base_framework.h"

#include "src/util/printf.h"

#include "src/mca/ras/ras.h"
/*
 * Global functions for MCA overall collective open and close
 */

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_ras_base_framework;
/* select a component */
PRRTE_EXPORT    int prrte_ras_base_select(void);

/*
 * globals that might be needed
 */
typedef struct prrte_ras_base_t {
    bool allocation_read;
    prrte_ras_base_module_t *active_module;
    int total_slots_alloc;
    int multiplier;
    bool launch_orted_on_hn;
} prrte_ras_base_t;

PRRTE_EXPORT extern prrte_ras_base_t prrte_ras_base;

PRRTE_EXPORT void prrte_ras_base_display_alloc(void);

PRRTE_EXPORT void prrte_ras_base_allocate(int fd, short args, void *cbdata);

PRRTE_EXPORT int prrte_ras_base_add_hosts(prrte_job_t *jdata);

END_C_DECLS

#endif
