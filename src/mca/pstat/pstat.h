/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2007 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2008 The Trustees of Indiana University.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 *
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/**
 * @file
 *
 * pstat (process statistics) framework component interface.
 *
 * Intent
 *
 * To support the ompi-top utility.
 *
 */

#ifndef PRRTE_MCA_PSTAT_H
#define PRRTE_MCA_PSTAT_H

#include "prrte_config.h"

#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/dss/dss_types.h"

BEGIN_C_DECLS

/**
 * Module initialization function.  Should return PRRTE_SUCCESS.
 */
typedef int (*prrte_pstat_base_module_init_fn_t)(void);

typedef int (*prrte_pstat_base_module_query_fn_t)(pid_t pid,
                                                 prrte_pstats_t *stats,
                                                 prrte_node_stats_t *nstats);

typedef int (*prrte_pstat_base_module_fini_fn_t)(void);

/**
 * Structure for pstat components.
 */
struct prrte_pstat_base_component_2_0_0_t {
    /** MCA base component */
    prrte_mca_base_component_t base_version;
    /** MCA base data */
    prrte_mca_base_component_data_t base_data;
};

/**
 * Convenience typedef
 */
typedef struct prrte_pstat_base_component_2_0_0_t prrte_pstat_base_component_2_0_0_t;
typedef struct prrte_pstat_base_component_2_0_0_t prrte_pstat_base_component_t;

/**
 * Structure for pstat modules
 */
struct prrte_pstat_base_module_1_0_0_t {
    prrte_pstat_base_module_init_fn_t    init;
    prrte_pstat_base_module_query_fn_t   query;
    prrte_pstat_base_module_fini_fn_t    finalize;
};

/**
 * Convenience typedef
 */
typedef struct prrte_pstat_base_module_1_0_0_t prrte_pstat_base_module_1_0_0_t;
typedef struct prrte_pstat_base_module_1_0_0_t prrte_pstat_base_module_t;


/**
 * Macro for use in components that are of type pstat
 */
#define PRRTE_PSTAT_BASE_VERSION_2_0_0 \
    PRRTE_MCA_BASE_VERSION_2_1_0("pstat", 2, 0, 0)

/* Global structure for accessing pstat functions */
PRRTE_EXPORT extern prrte_pstat_base_module_t prrte_pstat;

END_C_DECLS

#endif /* PRRTE_MCA_PSTAT_H */
