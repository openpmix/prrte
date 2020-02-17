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
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 * rmaps framework base functionality.
 */

#ifndef PRRTE_MCA_RMAPS_BASE_H
#define PRRTE_MCA_RMAPS_BASE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "types.h"

#include "src/class/prrte_list.h"
#include "src/util/printf.h"
#include "src/mca/mca.h"

#include "src/mca/base/prrte_mca_base_framework.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/rmaps/rmaps.h"

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRRTE_EXPORT extern prrte_mca_base_framework_t prrte_rmaps_base_framework;
/* select a component */
PRRTE_EXPORT    int prrte_rmaps_base_select(void);

/*
 * Global functions for MCA overall collective open and close
 */

/**
 * Struct to hold data global to the rmaps framework
 */
typedef struct {
    /* list of selected modules */
    prrte_list_t selected_modules;
    /* default ppr */
    char *ppr;
    /* cpus per rank */
    int cpus_per_rank;
    /* display the map after it is computed */
    bool display_map;
    /* slot list, if provided by user */
    char *slot_list;
    /* default mapping directives */
    prrte_mapping_policy_t mapping;
    prrte_ranking_policy_t ranking;
    /* device specification for min distance mapping */
    char *device;
    /* whether or not child jobs should inherit launch directives */
    bool inherit;
} prrte_rmaps_base_t;

/**
 * Global instance of rmaps-wide framework data
 */
PRRTE_EXPORT extern prrte_rmaps_base_t prrte_rmaps_base;

/**
 * Global MCA variables
 */
PRRTE_EXPORT extern bool prrte_rmaps_base_pernode;
PRRTE_EXPORT extern int prrte_rmaps_base_n_pernode;
PRRTE_EXPORT extern int prrte_rmaps_base_n_persocket;

/**
 * Select an rmaps component / module
 */
typedef struct {
    prrte_list_item_t super;
    int pri;
    prrte_rmaps_base_module_t *module;
    prrte_mca_base_component_t *component;
} prrte_rmaps_base_selected_module_t;
PRRTE_CLASS_DECLARATION(prrte_rmaps_base_selected_module_t);

/*
 * Map a job
 */
PRRTE_EXPORT void prrte_rmaps_base_map_job(int sd, short args, void *cbdata);
PRRTE_EXPORT int prrte_rmaps_base_assign_locations(prrte_job_t *jdata);

/**
 * Utility routines to get/set vpid mapping for the job
 */

PRRTE_EXPORT int prrte_rmaps_base_get_vpid_range(prrte_jobid_t jobid,
    prrte_vpid_t *start, prrte_vpid_t *range);
PRRTE_EXPORT int prrte_rmaps_base_set_vpid_range(prrte_jobid_t jobid,
    prrte_vpid_t start, prrte_vpid_t range);

/* pretty-print functions */
PRRTE_EXPORT char* prrte_rmaps_base_print_mapping(prrte_mapping_policy_t mapping);
PRRTE_EXPORT char* prrte_rmaps_base_print_ranking(prrte_ranking_policy_t ranking);

PRRTE_EXPORT int prrte_rmaps_base_prep_topology(hwloc_topology_t topo);

PRRTE_EXPORT int prrte_rmaps_base_filter_nodes(prrte_app_context_t *app,
                                               prrte_list_t *nodes,
                                               bool remove);

PRRTE_EXPORT int prrte_rmaps_base_set_mapping_policy(prrte_job_t *jdata,
                                                     prrte_mapping_policy_t *policy,
                                                     char **device, char *spec);
PRRTE_EXPORT int prrte_rmaps_base_set_ranking_policy(prrte_ranking_policy_t *policy,
                                                     prrte_mapping_policy_t mapping,
                                                     char *spec);

PRRTE_EXPORT void prrte_rmaps_base_display_map(prrte_job_t *jdata);

END_C_DECLS

#endif
