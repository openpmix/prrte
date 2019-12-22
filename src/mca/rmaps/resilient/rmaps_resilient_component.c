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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
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
#include "src/class/prrte_pointer_array.h"

#include "src/util/proc_info.h"
#include "src/util/show_help.h"

#include "src/mca/rmaps/base/rmaps_private.h"
#include "rmaps_resilient.h"

/*
 * Local functions
 */

static int prrte_rmaps_resilient_register(void);
static int prrte_rmaps_resilient_open(void);
static int prrte_rmaps_resilient_close(void);
static int prrte_rmaps_resilient_query(prrte_mca_base_module_t **module, int *priority);

static int my_priority;

prrte_rmaps_res_component_t prrte_rmaps_resilient_component = {
    {
        .base_version = {
            PRRTE_RMAPS_BASE_VERSION_2_0_0,

            .mca_component_name = "resilient",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),
            .mca_open_component = prrte_rmaps_resilient_open,
            .mca_close_component = prrte_rmaps_resilient_close,
            .mca_query_component = prrte_rmaps_resilient_query,
            .mca_register_component_params = prrte_rmaps_resilient_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};


/**
  * component register/open/close/init function
  */
static int prrte_rmaps_resilient_register (void)
{
    my_priority = 40;
    (void) prrte_mca_base_component_var_register (&prrte_rmaps_resilient_component.super.base_version,
                                            "priority", "Priority of the resilient rmaps component",
                                            PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    prrte_rmaps_resilient_component.fault_group_file = NULL;
    (void) prrte_mca_base_component_var_register (&prrte_rmaps_resilient_component.super.base_version,
                                            "fault_grp_file",
                                            "Filename that contains a description of fault groups for this system",
                                            PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                            PRRTE_INFO_LVL_9,
                                            PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                            &prrte_rmaps_resilient_component.fault_group_file);

    return PRRTE_SUCCESS;
}

static int prrte_rmaps_resilient_open(void)
{
    /* initialize globals */
    PRRTE_CONSTRUCT(&prrte_rmaps_resilient_component.fault_grps, prrte_list_t);

    return PRRTE_SUCCESS;
}


static int prrte_rmaps_resilient_query(prrte_mca_base_module_t **module, int *priority)
{
    *priority = my_priority;
    *module = (prrte_mca_base_module_t *)&prrte_rmaps_resilient_module;

    /* if a fault group file was provided, we should be first */
    if (NULL != prrte_rmaps_resilient_component.fault_group_file) {
        *priority = 1000;
    }

    return PRRTE_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int prrte_rmaps_resilient_close(void)
{
    prrte_list_item_t *item;

    while (NULL != (item = prrte_list_remove_first(&prrte_rmaps_resilient_component.fault_grps))) {
        PRRTE_RELEASE(item);
    }
    PRRTE_DESTRUCT(&prrte_rmaps_resilient_component.fault_grps);

    if (NULL != prrte_rmaps_resilient_component.fault_group_file) {
        free(prrte_rmaps_resilient_component.fault_group_file);
    }

    return PRRTE_SUCCESS;
}

static void ftgrp_res_construct(prrte_rmaps_res_ftgrp_t *ptr)
{
    ptr->ftgrp = -1;
    ptr->used = false;
    ptr->included = false;
    PRRTE_CONSTRUCT(&ptr->nodes, prrte_pointer_array_t);
    prrte_pointer_array_init(&ptr->nodes,
                            PRRTE_GLOBAL_ARRAY_BLOCK_SIZE,
                            PRRTE_GLOBAL_ARRAY_MAX_SIZE,
                            PRRTE_GLOBAL_ARRAY_BLOCK_SIZE);
}
static void ftgrp_res_destruct(prrte_rmaps_res_ftgrp_t *ptr)
{
    int n;
    prrte_node_t *node;

    for (n=0; n < ptr->nodes.size; n++) {
        if (NULL == (node = (prrte_node_t*)prrte_pointer_array_get_item(&ptr->nodes, n))) {
            continue;
        }
        PRRTE_RELEASE(node);
    }
    PRRTE_DESTRUCT(&ptr->nodes);
}
PRRTE_CLASS_INSTANCE(prrte_rmaps_res_ftgrp_t,
                   prrte_list_item_t,
                   ftgrp_res_construct,
                   ftgrp_res_destruct);

