/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"

#include "src/runtime/prte_globals.h"
#include "src/util/name_fns.h"

#include "ras_sim.h"
#include "src/mca/ras/base/ras_private.h"

/*
 * Local functions
 */
static int ras_sim_register(void);
static int ras_sim_component_query(prte_mca_base_module_t **module, int *priority);

prte_ras_sim_component_t prte_ras_simulator_component = {
    {
        /* First, the prte_mca_base_component_t struct containing meta
           information about the component itself */

        .base_version = {
            PRTE_RAS_BASE_VERSION_2_0_0,

            /* Component name and version */
            .mca_component_name = "simulator",
            PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                        PMIX_RELEASE_VERSION),
            .mca_query_component = ras_sim_component_query,
            .mca_register_component_params = ras_sim_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

static int ras_sim_register(void)
{
    prte_mca_base_component_t *component = &prte_ras_simulator_component.super.base_version;

    prte_ras_simulator_component.slots = "1";
    (void) prte_mca_base_component_var_register(
        component, "slots", "Comma-separated list of number of slots on each node to simulate",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_ras_simulator_component.slots);

    prte_ras_simulator_component.slots_max = "0";
    (void) prte_mca_base_component_var_register(
        component, "max_slots",
        "Comma-separated list of number of max slots on each node to simulate",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_ras_simulator_component.slots_max);

    prte_ras_simulator_component.num_nodes = NULL;
    (void) prte_mca_base_component_var_register(
        component, "num_nodes",
        "Comma-separated list of number of nodes to simulate for each topology",
        PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_ras_simulator_component.num_nodes);

    prte_ras_simulator_component.have_cpubind = true;
    (void) prte_mca_base_component_var_register(component, "have_cpubind",
                                                "Topology supports binding to cpus",
                                                PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &prte_ras_simulator_component.have_cpubind);

    prte_ras_simulator_component.have_membind = true;
    (void) prte_mca_base_component_var_register(component, "have_membind",
                                                "Topology supports binding to memory",
                                                PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &prte_ras_simulator_component.have_membind);
    return PRTE_SUCCESS;
}

static int ras_sim_component_query(prte_mca_base_module_t **module, int *priority)
{
    if (NULL != prte_ras_simulator_component.num_nodes) {
        *module = (prte_mca_base_module_t *) &prte_ras_sim_module;
        *priority = 1000;
        prte_ras_base.simulated = true;
        return PRTE_SUCCESS;
    }

    /* Sadly, no */
    *module = NULL;
    *priority = 0;
    return PRTE_ERROR;
}
