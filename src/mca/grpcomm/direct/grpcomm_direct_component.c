/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2016 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
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

#include "src/mca/mca.h"
#include "src/runtime/prrte_globals.h"
#include "src/mca/base/prrte_mca_base_var.h"

#include "src/util/proc_info.h"

#include "grpcomm_direct.h"

static int my_priority=5;  /* must be below "bad" module */
static int direct_open(void);
static int direct_close(void);
static int direct_query(prrte_mca_base_module_t **module, int *priority);
static int direct_register(void);

/*
 * Struct of function pointers that need to be initialized
 */
prrte_grpcomm_base_component_t prrte_grpcomm_direct_component = {
    .base_version = {
        PRRTE_GRPCOMM_BASE_VERSION_3_0_0,

        .mca_component_name = "direct",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),
        .mca_open_component = direct_open,
        .mca_close_component = direct_close,
        .mca_query_component = direct_query,
        .mca_register_component_params = direct_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int direct_register(void)
{
    prrte_mca_base_component_t *c = &prrte_grpcomm_direct_component.base_version;

    /* make the priority adjustable so users can select
     * direct for use by apps without affecting daemons
     */
    my_priority = 85;
    (void) prrte_mca_base_component_var_register(c, "priority",
                                           "Priority of the grpcomm direct component",
                                           PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           PRRTE_INFO_LVL_9,
                                           PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                           &my_priority);
    return PRRTE_SUCCESS;
}

/* Open the component */
static int direct_open(void)
{
    return PRRTE_SUCCESS;
}

static int direct_close(void)
{
    return PRRTE_SUCCESS;
}

static int direct_query(prrte_mca_base_module_t **module, int *priority)
{
    /* we are always available */
    *priority = my_priority;
    *module = (prrte_mca_base_module_t *)&prrte_grpcomm_direct_module;
    return PRRTE_SUCCESS;
}
