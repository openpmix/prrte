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
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
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
 * Resource Allocation (TM)
 */
#ifndef PRRTE_RAS_TM_H
#define PRRTE_RAS_TM_H

#include "prrte_config.h"
#include "src/mca/ras/ras.h"
#include "src/mca/ras/base/base.h"

BEGIN_C_DECLS

struct prrte_ras_tm_component_t {
    prrte_ras_base_component_t super;
    char *nodefile_dir;
    bool smp_mode;
};
typedef struct prrte_ras_tm_component_t prrte_ras_tm_component_t;

PRRTE_EXPORT extern prrte_ras_tm_component_t prrte_ras_tm_component;
PRRTE_EXPORT extern prrte_ras_base_module_t prrte_ras_tm_module;

END_C_DECLS

#endif
