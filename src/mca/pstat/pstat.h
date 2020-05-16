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
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
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

#ifndef PRTE_MCA_PSTAT_H
#define PRTE_MCA_PSTAT_H

#include "prte_config.h"

#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/dss/dss_types.h"

BEGIN_C_DECLS

/**
 * Module initialization function.  Should return PRTE_SUCCESS.
 */
typedef int (*prte_pstat_base_module_init_fn_t)(void);

typedef int (*prte_pstat_base_module_query_fn_t)(pid_t pid,
                                                 prte_pstats_t *stats,
                                                 prte_node_stats_t *nstats);

typedef int (*prte_pstat_base_module_fini_fn_t)(void);

/**
 * Structure for pstat components.
 */
struct prte_pstat_base_component_2_0_0_t {
    /** MCA base component */
    prte_mca_base_component_t base_version;
    /** MCA base data */
    prte_mca_base_component_data_t base_data;
};

/**
 * Convenience typedef
 */
typedef struct prte_pstat_base_component_2_0_0_t prte_pstat_base_component_2_0_0_t;
typedef struct prte_pstat_base_component_2_0_0_t prte_pstat_base_component_t;

/**
 * Structure for pstat modules
 */
struct prte_pstat_base_module_1_0_0_t {
    prte_pstat_base_module_init_fn_t    init;
    prte_pstat_base_module_query_fn_t   query;
    prte_pstat_base_module_fini_fn_t    finalize;
};

/**
 * Convenience typedef
 */
typedef struct prte_pstat_base_module_1_0_0_t prte_pstat_base_module_1_0_0_t;
typedef struct prte_pstat_base_module_1_0_0_t prte_pstat_base_module_t;


/**
 * Macro for use in components that are of type pstat
 */
#define PRTE_PSTAT_BASE_VERSION_2_0_0 \
    PRTE_MCA_BASE_VERSION_2_1_0("pstat", 2, 0, 0)

/* Global structure for accessing pstat functions */
PRTE_EXPORT extern prte_pstat_base_module_t prte_pstat;

END_C_DECLS

#endif /* PRTE_MCA_PSTAT_H */
