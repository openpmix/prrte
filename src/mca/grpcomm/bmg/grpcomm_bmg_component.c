/*
 * Copyright (c) 2016-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * Copyright (c) 2020      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include "src/mca/base/prte_mca_base_var.h"
#include "src/mca/mca.h"
#include "src/runtime/prte_globals.h"

#include "src/util/proc_info.h"

#include "grpcomm_bmg.h"

static int my_priority = 5;
static int bmg_open(void);
static int bmg_close(void);
static int bmg_query(prte_mca_base_module_t **module, int *priority);
static int bmg_register(void);

/*
 * Struct of function pointers that need to be initialized
 */
prte_grpcomm_base_component_t prte_grpcomm_bmg_component = {
    .base_version = {
        PRTE_GRPCOMM_BASE_VERSION_3_0_0,

        .mca_component_name = "bmg",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                PMIX_RELEASE_VERSION),
        .mca_open_component = bmg_open,
        .mca_close_component = bmg_close,
        .mca_query_component = bmg_query,
        .mca_register_component_params = bmg_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int bmg_register(void)
{
    prte_mca_base_component_t *c = &prte_grpcomm_bmg_component.base_version;

    /* make the priority adjustable so users can select
     * bmg for use by apps without affecting daemons
     */
    my_priority = 50;
    (void) prte_mca_base_component_var_register(c, "priority",
                                                "Priority of the grpcomm bmg component",
                                                PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);
    return PRTE_SUCCESS;
}

/* Open the component */
static int bmg_open(void)
{
    return PRTE_SUCCESS;
}

static int bmg_close(void)
{
    return PRTE_SUCCESS;
}

static int bmg_query(prte_mca_base_module_t **module, int *priority)
{
    if (prte_enable_ft) {
        *priority = my_priority;
        *module = (prte_mca_base_module_t *) &prte_grpcomm_bmg_module;
        return PRTE_SUCCESS;
    }

    *priority = 0;
    *module = NULL;
    return PRTE_ERROR;
}
