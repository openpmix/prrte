/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2008 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      UT-Battelle, LLC. All rights reserved.
 * Copyright (c) 2011      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRRTE_RAS_PRIVATE_H
#define PRRTE_RAS_PRIVATE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "constants.h"
#include "types.h"

#include "src/class/prrte_list.h"

#include "src/mca/ras/ras.h"
#include "src/mca/ras/base/base.h"


BEGIN_C_DECLS

/**
* Add the specified node definitions to the registry
 */
PRRTE_EXPORT int prrte_ras_base_node_insert(prrte_list_t*, prrte_job_t*);

END_C_DECLS

#endif
