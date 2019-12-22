/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 *
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
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
#ifndef PRRTE_RMAPS_RESILIENT_H
#define PRRTE_RMAPS_RESILIENT_H

#include "prrte_config.h"

#include "src/class/prrte_pointer_array.h"

#include "src/mca/rmaps/rmaps.h"

BEGIN_C_DECLS

struct prrte_rmaps_res_component_t {
    prrte_rmaps_base_component_t super;
    char *fault_group_file;
    prrte_list_t fault_grps;
};
typedef struct prrte_rmaps_res_component_t prrte_rmaps_res_component_t;

typedef struct {
    prrte_list_item_t super;
    int ftgrp;
    bool used;
    bool included;
    prrte_pointer_array_t nodes;
} prrte_rmaps_res_ftgrp_t;
PRRTE_EXPORT PRRTE_CLASS_DECLARATION(prrte_rmaps_res_ftgrp_t);

PRRTE_MODULE_EXPORT extern prrte_rmaps_res_component_t prrte_rmaps_resilient_component;
extern prrte_rmaps_base_module_t prrte_rmaps_resilient_module;


END_C_DECLS

#endif
