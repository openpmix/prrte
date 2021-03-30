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
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2012 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRTE_MCA_RMAPS_PRIVATE_H
#define PRTE_MCA_RMAPS_PRIVATE_H

/*
 * includes
 */
#include "prte_config.h"
#include "types.h"

#include "src/runtime/prte_globals.h"

#include "src/mca/rmaps/rmaps.h"

BEGIN_C_DECLS

/*
 * Base API functions
 */

/* LOCAL FUNCTIONS for use by RMAPS components */

PRTE_EXPORT int prte_rmaps_base_get_target_nodes(prte_list_t *node_list, int32_t *total_num_slots,
                                                 prte_app_context_t *app,
                                                 prte_mapping_policy_t policy, bool initial_map,
                                                 bool silent);

PRTE_EXPORT prte_proc_t *prte_rmaps_base_setup_proc(prte_job_t *jdata, prte_node_t *node,
                                                    prte_app_idx_t idx);

PRTE_EXPORT prte_node_t *prte_rmaps_base_get_starting_point(prte_list_t *node_list,
                                                            prte_job_t *jdata);

PRTE_EXPORT int prte_rmaps_base_compute_vpids(prte_job_t *jdata);

PRTE_EXPORT int prte_rmaps_base_compute_local_ranks(prte_job_t *jdata);

PRTE_EXPORT int prte_rmaps_base_compute_bindings(prte_job_t *jdata);

PRTE_EXPORT void prte_rmaps_base_update_local_ranks(prte_job_t *jdata, prte_node_t *oldnode,
                                                    prte_node_t *newnode, prte_proc_t *newproc);

PRTE_EXPORT int prte_rmaps_base_rearrange_map(prte_app_context_t *app, prte_job_map_t *map,
                                              prte_list_t *procs);

END_C_DECLS

#endif
