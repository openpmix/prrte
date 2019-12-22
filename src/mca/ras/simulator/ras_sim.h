/*
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PRRTE_RAS_SIM_H
#define PRRTE_RAS_SIM_H

#include "prrte_config.h"
#include "src/mca/ras/ras.h"
#include "src/mca/ras/base/base.h"

BEGIN_C_DECLS

struct prrte_ras_sim_component_t {
    prrte_ras_base_component_t super;
    char *num_nodes;
    char * slots;
    char * slots_max;
    char *topofiles;
    char *topologies;
    bool have_cpubind;
    bool have_membind;
};
typedef struct prrte_ras_sim_component_t prrte_ras_sim_component_t;

PRRTE_EXPORT extern prrte_ras_sim_component_t prrte_ras_simulator_component;
PRRTE_EXPORT extern prrte_ras_base_module_t prrte_ras_sim_module;

END_C_DECLS

#endif
