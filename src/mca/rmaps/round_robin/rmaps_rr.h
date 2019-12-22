/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      Cisco Systems, Inc.  All rights reserved
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
 * Resource Mapping
 */
#ifndef PRRTE_RMAPS_RR_H
#define PRRTE_RMAPS_RR_H

#include "prrte_config.h"

#include "src/hwloc/hwloc-internal.h"
#include "src/class/prrte_list.h"

#include "src/mca/rmaps/rmaps.h"

BEGIN_C_DECLS

PRRTE_MODULE_EXPORT extern prrte_rmaps_base_component_t prrte_rmaps_round_robin_component;
extern prrte_rmaps_base_module_t prrte_rmaps_round_robin_module;

PRRTE_MODULE_EXPORT int prrte_rmaps_rr_bynode(prrte_job_t *jdata,
                                              prrte_app_context_t *app,
                                              prrte_list_t *node_list,
                                              prrte_std_cntr_t num_slots,
                                              prrte_vpid_t nprocs);
PRRTE_MODULE_EXPORT int prrte_rmaps_rr_byslot(prrte_job_t *jdata,
                                              prrte_app_context_t *app,
                                              prrte_list_t *node_list,
                                              prrte_std_cntr_t num_slots,
                                              prrte_vpid_t nprocs);

PRRTE_MODULE_EXPORT int prrte_rmaps_rr_byobj(prrte_job_t *jdata, prrte_app_context_t *app,
                                             prrte_list_t *node_list,
                                             prrte_std_cntr_t num_slots,
                                             prrte_vpid_t num_procs,
                                             hwloc_obj_type_t target, unsigned cache_level);

PRRTE_MODULE_EXPORT int prrte_rmaps_rr_assign_root_level(prrte_job_t *jdata);

PRRTE_MODULE_EXPORT int prrte_rmaps_rr_assign_byobj(prrte_job_t *jdata,
                                                    hwloc_obj_type_t target,
                                                    unsigned cache_level);


END_C_DECLS

#endif
