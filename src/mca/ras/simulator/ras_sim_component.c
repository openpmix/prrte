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
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"
#include "src/mca/if/if.h"

#include "src/util/name_fns.h"
#include "src/runtime/prrte_globals.h"

#include "src/mca/ras/base/ras_private.h"
#include "ras_sim.h"

/*
 * Local functions
 */
static int ras_sim_register(void);
static int ras_sim_component_query(prrte_mca_base_module_t **module, int *priority);


prrte_ras_sim_component_t prrte_ras_simulator_component = {
    {
        /* First, the prrte_mca_base_component_t struct containing meta
           information about the component itself */

        .base_version = {
            PRRTE_RAS_BASE_VERSION_2_0_0,

            /* Component name and version */
            .mca_component_name = "simulator",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),
            .mca_query_component = ras_sim_component_query,
            .mca_register_component_params = ras_sim_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};


static int ras_sim_register(void)
{
    prrte_mca_base_component_t *component = &prrte_ras_simulator_component.super.base_version;

    prrte_ras_simulator_component.slots = "1";
    (void) prrte_mca_base_component_var_register (component, "slots",
                                            "Comma-separated list of number of slots on each node to simulate",
                                            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_simulator_component.slots);

    prrte_ras_simulator_component.slots_max = "0";
    (void) prrte_mca_base_component_var_register (component, "max_slots",
                                            "Comma-separated list of number of max slots on each node to simulate",
                                            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_simulator_component.slots_max);
    prrte_ras_simulator_component.num_nodes = NULL;
    (void) prrte_mca_base_component_var_register (component, "num_nodes",
                                            "Comma-separated list of number of nodes to simulate for each topology",
                                            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_simulator_component.num_nodes);
    prrte_ras_simulator_component.topofiles = NULL;
    (void) prrte_mca_base_component_var_register (component, "topo_files",
                                            "Comma-separated list of files containing xml topology descriptions for simulated nodes",
                                            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_simulator_component.topofiles);
    prrte_ras_simulator_component.topologies = NULL;
    (void) prrte_mca_base_component_var_register (component, "topologies",
                                            "Comma-separated list of topology descriptions for simulated nodes",
                                            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_simulator_component.topologies);
    prrte_ras_simulator_component.have_cpubind = true;
    (void) prrte_mca_base_component_var_register (component, "have_cpubind",
                                            "Topology supports binding to cpus",
                                            PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_simulator_component.have_cpubind);
    prrte_ras_simulator_component.have_membind = true;
    (void) prrte_mca_base_component_var_register (component, "have_membind",
                                            "Topology supports binding to memory",
                                            PRRTE_MCA_BASE_VAR_TYPE_BOOL,NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_ras_simulator_component.have_membind);
    return PRRTE_SUCCESS;
}


static int ras_sim_component_query(prrte_mca_base_module_t **module, int *priority)
{
    if (NULL != prrte_ras_simulator_component.num_nodes) {
        *module = (prrte_mca_base_module_t *) &prrte_ras_sim_module;
        *priority = 1000;
        /* cannot launch simulated nodes or resolve their names to addresses */
        prrte_do_not_launch = true;
        prrte_if_do_not_resolve = true;
        return PRRTE_SUCCESS;
    }

    /* Sadly, no */
    *module = NULL;
    *priority = 0;
    return PRRTE_ERROR;
}
