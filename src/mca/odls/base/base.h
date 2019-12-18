/*
 * Copyright (c) 2004-2006 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2011 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013      Los Alamos National Security, LLC.  All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef MCA_ODLS_BASE_H
#define MCA_ODLS_BASE_H

/*
 * includes
 */
#include "prrte_config.h"

#include "src/mca/mca.h"

#include "src/mca/odls/odls.h"


BEGIN_C_DECLS

/*
 * MCA framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_odls_base_framework;
/*
 * Select an available component.
 */
PRRTE_EXPORT int prrte_odls_base_select(void);

PRRTE_EXPORT void prrte_odls_base_start_threads(prrte_job_t *jdata);

PRRTE_EXPORT void prrte_odls_base_harvest_threads(void);

END_C_DECLS
#endif
