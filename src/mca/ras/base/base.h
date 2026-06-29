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
 * Copyright (c) 2011-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/** @file:
 */

#ifndef PRTE_MCA_RAS_BASE_H
#define PRTE_MCA_RAS_BASE_H

/*
 * includes
 */
#include "prte_config.h"
#include "src/mca/base/pmix_mca_base_framework.h"

#include "src/mca/ras/ras.h"
#include "src/runtime/prte_globals.h"
#include "src/util/pmix_printf.h"
/*
 * Global functions for MCA overall collective open and close
 */

BEGIN_C_DECLS

/*
 * MCA Framework
 */
PRTE_EXPORT extern pmix_mca_base_framework_t prte_ras_base_framework;
/* select a component */
PRTE_EXPORT int prte_ras_base_select(void);

/*
 * globals that might be needed
 */
typedef struct prte_ras_base_t {
    /* list of selected modules */
    pmix_list_t selected_modules;
    int total_slots_alloc;
    int multiplier;
    bool launch_orted_on_hn;
    bool simulated;
    /* set once the DVM's base allocation has been established (the first
     * allocation to complete). Used to detect that a subsequent job must
     * reuse the existing allocation rather than re-run discovery. This is
     * independent of whether the HNP node itself is part of the allocation. */
    bool allocation_established;
} prte_ras_base_t;

PRTE_EXPORT extern prte_ras_base_t prte_ras_base;

typedef struct {
    pmix_list_item_t super;
    int pri;
    prte_ras_base_module_t *module;
    pmix_mca_base_component_t *component;
} prte_ras_base_selected_module_t;
PMIX_CLASS_DECLARATION(prte_ras_base_selected_module_t);

/**
 * Add the specified node definitions to the registry
 */
PRTE_EXPORT int prte_ras_base_node_insert(pmix_list_t *, prte_job_t *);

PRTE_EXPORT void prte_ras_base_display_alloc(prte_job_t *jdata);

PRTE_EXPORT void prte_ras_base_display_cpus(prte_job_t *jdata, char *nodelist);

PRTE_EXPORT void prte_ras_base_allocate(int fd, short args, void *cbdata);

PRTE_EXPORT void prte_ras_base_modify(int fd, short args, void *cbdata);

PRTE_EXPORT void prte_ras_base_release_allocation(prte_session_t *session);

/* Tear down a reservation: drop its hold on its nodes (clearing the
 * node->session backpointer so the nodes revert to the default pool) and
 * deregister it so it can no longer be targeted. When return_to_scheduler is
 * true, member nodes carrying a daemon are additionally shrunk out of the DVM
 * and handed back to the scheduler; otherwise the nodes simply become
 * unreserved within the session. */
PRTE_EXPORT void prte_ras_base_teardown_reservation(prte_session_t *session,
                                                    bool return_to_scheduler);

/* Apply the namespace-termination inheritance disposition of every reservation
 * when the namespace owning jdata terminates. NONE/DEFAULT fire when the owning
 * namespace exits; CHILD/CHILD_DEFAULT fire when the last derived child of the
 * owning namespace exits. The *_DEFAULT variants unreserve into the session
 * rather than returning nodes to the scheduler. */
PRTE_EXPORT void prte_ras_base_check_reservations_on_term(prte_job_t *jdata);

PRTE_EXPORT int prte_ras_base_add_hosts(prte_job_t *jdata);

PRTE_EXPORT char *prte_ras_base_flag_string(prte_node_t *node);

PRTE_EXPORT void prte_ras_base_complete_request(prte_pmix_server_req_t *req);

END_C_DECLS

#endif
