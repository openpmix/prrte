/*
 * Copyright (c) 2009      Cisco Systems, Inc.  All rights reserved.
 *
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
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
#ifndef PRTE_RMAPS_RESILIENT_H
#define PRTE_RMAPS_RESILIENT_H

#include "prte_config.h"

#include "src/class/prte_pointer_array.h"

#include "src/mca/rmaps/rmaps.h"

BEGIN_C_DECLS

struct prte_rmaps_res_component_t {
    prte_rmaps_base_component_t super;
    char *fault_group_file;
    prte_list_t fault_grps;
};
typedef struct prte_rmaps_res_component_t prte_rmaps_res_component_t;

typedef struct {
    prte_list_item_t super;
    int ftgrp;
    bool used;
    bool included;
    prte_pointer_array_t nodes;
} prte_rmaps_res_ftgrp_t;
PRTE_EXPORT PRTE_CLASS_DECLARATION(prte_rmaps_res_ftgrp_t);

PRTE_EXPORT extern prte_rmaps_res_component_t prte_rmaps_rcm_component;
extern prte_rmaps_base_module_t prte_rmaps_rcm_module;


END_C_DECLS

#endif
