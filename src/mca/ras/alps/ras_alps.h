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
 * Copyright (c) 2008      UT-Battelle, LLC
 * Copyright (c) 2011      Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * Resource Allocation (ALPS)
 */
#ifndef PRRTE_RAS_ALPS_H
#define PRRTE_RAS_ALPS_H

#include "prrte_config.h"
#include "src/mca/ras/ras.h"
#include "src/mca/ras/base/base.h"

BEGIN_C_DECLS

PRRTE_EXPORT extern prrte_ras_base_component_t prrte_ras_alps_component;
PRRTE_EXPORT extern prrte_ras_base_module_t prrte_ras_alps_module;
PRRTE_EXPORT int prrte_ras_alps_get_appinfo_attempts(int *attempts);
PRRTE_EXPORT extern unsigned long int prrte_ras_alps_res_id;

END_C_DECLS

#endif
