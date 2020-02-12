/*
 * Copyright (c) 2016-2020 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
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

#include "grpcomm_bmg.h"

static int my_priority=5;
static int bmg_open(void);
static int bmg_close(void);
static int bmg_query(prrte_mca_base_module_t **module, int *priority);
static int bmg_register(void);

/*
 * Struct of function pointers that need to be initialized
 */
prrte_grpcomm_base_component_t prrte_grpcomm_bmg_component = {
    .base_version = {
        PRRTE_GRPCOMM_BASE_VERSION_3_0_0,

        .mca_component_name = "bmg",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                PRRTE_RELEASE_VERSION),
        .mca_open_component = bmg_open,
        .mca_close_component = bmg_close,
        .mca_query_component = bmg_query,
        .mca_register_component_params = bmg_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int bmg_register(void)
{
    prrte_mca_base_component_t *c = &prrte_grpcomm_bmg_component.base_version;

    /* make the priority adjustable so users can select
     * bmg for use by apps without affecting daemons
     */
    my_priority = 50;
    (void) prrte_mca_base_component_var_register(c, "priority",
                                           "Priority of the grpcomm bmg component",
                                           PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           PRRTE_INFO_LVL_9,
                                           PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                           &my_priority);
    return PRRTE_SUCCESS;
}

/* Open the component */
static int bmg_open(void)
{
    return PRRTE_SUCCESS;
}

static int bmg_close(void)
{
    return PRRTE_SUCCESS;
}

static int bmg_query(prrte_mca_base_module_t **module, int *priority)
{
    *priority = my_priority;
    *module = (prrte_mca_base_module_t *)&prrte_grpcomm_bmg_module;
    return PRRTE_SUCCESS;
}

