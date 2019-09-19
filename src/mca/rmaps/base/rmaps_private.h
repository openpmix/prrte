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
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2017-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRRTE_MCA_RMAPS_PRIVATE_H
#define PRRTE_MCA_RMAPS_PRIVATE_H

/*
 * includes
 */
#include "prrte_config.h"
#include "types.h"

#include "src/runtime/prrte_globals.h"

#include "src/mca/rmaps/rmaps.h"

BEGIN_C_DECLS

/*
 * Base API functions
 */

/* LOCAL FUNCTIONS for use by RMAPS components */

PRRTE_EXPORT int prrte_rmaps_base_get_target_nodes(prrte_list_t* node_list,
                                                   prrte_std_cntr_t *total_num_slots,
                                                   prrte_app_context_t *app,
                                                   prrte_mapping_policy_t policy,
                                                   bool initial_map, bool silent);

PRRTE_EXPORT prrte_proc_t* prrte_rmaps_base_setup_proc(prrte_job_t *jdata,
                                                      prrte_node_t *node,
                                                      prrte_app_idx_t idx);

PRRTE_EXPORT prrte_node_t* prrte_rmaps_base_get_starting_point(prrte_list_t *node_list,
                                                              prrte_job_t *jdata);

PRRTE_EXPORT int prrte_rmaps_base_compute_vpids(prrte_job_t *jdata);

PRRTE_EXPORT int prrte_rmaps_base_compute_local_ranks(prrte_job_t *jdata);

PRRTE_EXPORT int prrte_rmaps_base_compute_bindings(prrte_job_t *jdata);

PRRTE_EXPORT void prrte_rmaps_base_update_local_ranks(prrte_job_t *jdata, prrte_node_t *oldnode,
                                                      prrte_node_t *newnode, prrte_proc_t *newproc);

PRRTE_EXPORT int prrte_rmaps_base_rearrange_map(prrte_app_context_t *app, prrte_job_map_t *map, prrte_list_t *procs);

END_C_DECLS

#endif
