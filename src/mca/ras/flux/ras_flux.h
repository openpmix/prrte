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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * Copyright (c) 2025-2026 Triad National Security, LLC. All rights
 *                         reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * Resource Allocation (Flux)
 */
#ifndef PRTE_RAS_FLUX_H
#define PRTE_RAS_FLUX_H

#include "prte_config.h"
#include "src/mca/ras/base/base.h"
#include "src/mca/ras/ras.h"

BEGIN_C_DECLS

struct prte_mca_ras_flux_component_t {
    prte_ras_base_component_t super;
    char *flux_broker_uri;
    int flux_open_flags;
};
typedef struct prte_mca_ras_flux_component_t prte_mca_ras_flux_component_t;

PRTE_EXPORT extern prte_mca_ras_flux_component_t prte_mca_ras_flux_component;
PRTE_EXPORT extern prte_ras_base_module_t prte_ras_flux_module;

END_C_DECLS

#endif
