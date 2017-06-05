/*
 * Copyright (c) 2016-2018 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"

#include "orte/mca/mca.h"
#include "opal/runtime/opal_params.h"
#include "opal/mca/base/mca_base_var.h"

#include "orte/util/proc_info.h"

#include "grpcomm_bmg.h"

static int my_priority=5;
static int bmg_open(void);
static int bmg_close(void);
static int bmg_query(mca_base_module_t **module, int *priority);
static int bmg_register(void);

/*
 * Struct of function pointers that need to be initialized
 */
orte_grpcomm_base_component_t mca_grpcomm_bmg_component = {
    .base_version = {
        ORTE_GRPCOMM_BASE_VERSION_3_0_0,

        .mca_component_name = "bmg",
        MCA_BASE_MAKE_VERSION(component, ORTE_MAJOR_VERSION, ORTE_MINOR_VERSION,
                ORTE_RELEASE_VERSION),
        .mca_open_component = bmg_open,
        .mca_close_component = bmg_close,
        .mca_query_component = bmg_query,
        .mca_register_component_params = bmg_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int bmg_register(void)
{
    mca_base_component_t *c = &mca_grpcomm_bmg_component.base_version;

    /* make the priority adjustable so users can select
     * bmg for use by apps without affecting daemons
     */
    my_priority = 50;
    (void) mca_base_component_var_register(c, "priority",
                                           "Priority of the grpcomm bmg component",
                                           MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           OPAL_INFO_LVL_9,
                                           MCA_BASE_VAR_SCOPE_READONLY,
                                           &my_priority);
    return ORTE_SUCCESS;
}

/* Open the component */
static int bmg_open(void)
{
    return ORTE_SUCCESS;
}

static int bmg_close(void)
{
    return ORTE_SUCCESS;
}

static int bmg_query(mca_base_module_t **module, int *priority)
{
    *priority = my_priority;
    *module = (mca_base_module_t *)&orte_grpcomm_bmg_module;
    return ORTE_SUCCESS;
}

