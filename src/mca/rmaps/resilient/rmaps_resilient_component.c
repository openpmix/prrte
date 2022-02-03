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
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2022      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/base.h"
#include "src/util/show_help.h"
#include "src/mca/rmaps/base/base.h"
#include "rmaps_resilient.h"

/*
 * Local functions
 */

static int resilient_register(void);
static int resilient_open(void);
static int resilient_close(void);
static int resilient_query(prte_mca_base_module_t **module, int *priority);

static int my_priority;

prte_rmaps_res_component_t prte_rmaps_resilient_component = {
    {
        .base_version = {
            PRTE_RMAPS_BASE_VERSION_2_0_0,

            .mca_component_name = "resilient",
            PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                       PRTE_RELEASE_VERSION),
            .mca_open_component = resilient_open,
            .mca_close_component = resilient_close,
            .mca_query_component = resilient_query,
            .mca_register_component_params = resilient_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

static int my_priority;

/**
  * component register/open/close/init function
  */
static int resilient_register (void)
{
    my_priority = 40;
    (void) prte_mca_base_component_var_register (&prte_rmaps_resilient_component.super.base_version,
                                            "priority", "Priority of the resilient rmaps component",
                                                 PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                                 PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                 PRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    prte_rmaps_resilient_component.fault_group_file = NULL;
    (void) prte_mca_base_component_var_register (&prte_rmaps_resilient_component.super.base_version,
                                             "fault_grp_file",
                                             "Filename that contains a description of fault groups for this system",
                                             PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0,
                                             PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                             PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                             &prte_rmaps_resilient_component.fault_group_file);

    return PRTE_SUCCESS;
}

static int resilient_open(void)
{
    /* initialize globals */
    PRTE_CONSTRUCT(&prte_rmaps_resilient_component.fault_grps, prte_list_t);

    return PRTE_SUCCESS;
}


static int resilient_query(prte_mca_base_module_t **module, int *priority)
{
    *priority = my_priority;
    *module = (prte_mca_base_module_t *)&prte_rmaps_resilient_module;

    /* if a fault group file was provided, we should be first */
    if (NULL != prte_rmaps_resilient_component.fault_group_file) {
        *priority = 1000;
    }

    return PRTE_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int resilient_close(void)
{
    prte_list_item_t *item;

    PRTE_LIST_DESTRUCT(&prte_rmaps_resilient_component.fault_grps);

    if (NULL != prte_rmaps_resilient_component.fault_group_file) {
        free(prte_rmaps_resilient_component.fault_group_file);
    }

    return PRTE_SUCCESS;
}

static void ftgrp_res_construct(prte_rmaps_res_ftgrp_t *ptr)
{
    ptr->ftgrp = -1;
    ptr->used = false;
    ptr->included = false;
    PRTE_CONSTRUCT(&ptr->nodes, prte_pointer_array_t);
    prte_pointer_array_init(&ptr->nodes,
                            PRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                            PRTE_GLOBAL_ARRAY_MAX_SIZE,
                            PRTE_GLOBAL_ARRAY_BLOCK_SIZE);
}
static void ftgrp_res_destruct(prte_rmaps_res_ftgrp_t *ptr)
{
    int n;
    prte_node_t *node;

    for (n=0; n < ptr->nodes.size; n++) {
        if (NULL == (node = (prte_node_t*)prte_pointer_array_get_item(&ptr->nodes, n))) {
            continue;
        }
        PRTE_RELEASE(node);
    }
    PRTE_DESTRUCT(&ptr->nodes);
}
PRTE_CLASS_INSTANCE(prte_rmaps_res_ftgrp_t,
                    prte_list_item_t,
                    ftgrp_res_construct,
                    ftgrp_res_destruct);

