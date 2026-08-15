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
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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
#ifndef PRTE_RMAPS_RR_H
#define PRTE_RMAPS_RR_H

#include "prte_config.h"

#include "src/class/pmix_list.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/mca/rmaps/rmaps.h"

BEGIN_C_DECLS

PRTE_MODULE_EXPORT extern prte_rmaps_base_component_t prte_mca_rmaps_round_robin_component;
extern prte_rmaps_base_module_t prte_rmaps_round_robin_module;

PRTE_MODULE_EXPORT int prte_rmaps_rr_bynode(prte_job_t *jdata, prte_app_context_t *app,
                                            pmix_list_t *node_list, int32_t num_slots,
                                            pmix_rank_t nprocs, prte_rmaps_options_t *options);
PRTE_MODULE_EXPORT int prte_rmaps_rr_byslot(prte_job_t *jdata, prte_app_context_t *app,
                                            pmix_list_t *node_list, int32_t num_slots,
                                            pmix_rank_t nprocs, prte_rmaps_options_t *options);

/* How a node's placement targets are enumerated.
 *
 * "Map one proc per target, wrapping until the node is full or the procs run
 * out" is the same loop whatever the targets are; only the way they are
 * listed differs.  byobj lists the hwloc objects of a single type, and that
 * is the only enumerator today - but the loop it drives is the subtlest one
 * in this component (the redo/check_avail interplay and the oversubscribe
 * second pass have both been the source of real bugs), so a second kind of
 * target is added by writing an enumerator rather than by copying the loop.
 *
 * "ctx" is enumerator-private per-node state: "begin" may allocate it and
 * "end" releases it.  An enumerator that needs neither leaves both NULL, in
 * which case "ctx" is always NULL. */
typedef struct {
    /* Called once per node before anything is placed on it.  Returns
     * PRTE_SUCCESS, or an error that fails the map.  May be NULL. */
    int (*begin)(prte_node_t *node, prte_rmaps_options_t *opts, void **ctx);
    /* How many targets this node offers.  Zero is not an error here - the
     * caller decides what it means. */
    unsigned (*count)(prte_node_t *node, prte_rmaps_options_t *opts, void *ctx);
    /* The j-th target, or NULL if there is no such target. */
    hwloc_obj_t (*item)(prte_node_t *node, prte_rmaps_options_t *opts, void *ctx,
                        unsigned j);
    /* Called after a proc has been placed against the j-th target, so the
     * enumerator can record what that target was.  May be NULL. */
    void (*placed)(prte_proc_t *proc, prte_rmaps_options_t *opts, void *ctx,
                   unsigned j);
    /* Release whatever "begin" allocated.  May be NULL. */
    void (*end)(void *ctx);
    /* What a target is called, for diagnostics ("core", "numa", ...). */
    const char *name;
    /* When true, a target takes at most one proc: the loop lays one proc on
     * each target and stops rather than coming round again.  A target that
     * cannot be shared - a device, which is assigned rather than subdivided
     * - sets this unless the user has allowed overloading. */
    bool nowrap;
} prte_rmaps_target_enum_t;

PRTE_MODULE_EXPORT int prte_rmaps_rr_byobj(prte_job_t *jdata,
                                           prte_app_context_t *app,
                                           pmix_list_t *node_list,
                                           int32_t num_slots,
                                           pmix_rank_t num_procs,
                                           prte_rmaps_options_t *options);

/* The shared placement loop.  byobj is a thin wrapper around this. */
PRTE_MODULE_EXPORT int prte_rmaps_rr_map_targets(prte_job_t *jdata,
                                                 prte_app_context_t *app,
                                                 pmix_list_t *node_list,
                                                 int32_t num_slots,
                                                 pmix_rank_t num_procs,
                                                 prte_rmaps_options_t *options,
                                                 prte_rmaps_target_enum_t *tgts);

PRTE_MODULE_EXPORT int prte_rmaps_rr_bydevice(prte_job_t *jdata, prte_app_context_t *app,
                                              pmix_list_t *node_list, int32_t num_slots,
                                              pmix_rank_t num_procs,
                                              prte_rmaps_options_t *options);

PRTE_MODULE_EXPORT int prte_rmaps_rr_bycpu(prte_job_t *jdata, prte_app_context_t *app,
                                           pmix_list_t *node_list, int32_t num_slots,
                                           pmix_rank_t num_procs, prte_rmaps_options_t *options);

END_C_DECLS

#endif
