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
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
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
 * Resource Allocation (SLURM)
 */
#ifndef PRRTE_RAS_SLURM_H
#define PRRTE_RAS_SLURM_H

#include "prrte_config.h"
#include "src/mca/ras/ras.h"
#include "src/mca/ras/base/base.h"

BEGIN_C_DECLS

typedef struct {
    prrte_ras_base_component_t super;
    int timeout;
    bool dyn_alloc_enabled;
    char *config_file;
    bool rolling_alloc;
    bool use_all;
} prrte_ras_slurm_component_t;
PRRTE_EXPORT extern prrte_ras_slurm_component_t prrte_ras_slurm_component;

PRRTE_EXPORT extern prrte_ras_base_module_t prrte_ras_slurm_module;

END_C_DECLS

#endif
