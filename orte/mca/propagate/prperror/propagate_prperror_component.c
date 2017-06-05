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
#include "opal/util/output.h"

#include "orte/mca/propagate/propagate.h"
#include "orte/mca/propagate/base/base.h"
#include "propagate_prperror.h"

/*
 * Public string for version number
 */
const char *orte_propagate_prperror_component_version_string =
"ORTE PROPAGATE prperror MCA component version " ORTE_VERSION;

/*
 * Local functionality
 */
static int propagate_prperror_register(void);
static int propagate_prperror_open(void);
static int propagate_prperror_close(void);
static int propagate_prperror_component_query(mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
orte_propagate_base_component_t mca_propagate_prperror_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component prperror
     */
    .base_version = {
        ORTE_PROPAGATE_BASE_VERSION_3_0_0,
        /* Component name and version */
        .mca_component_name = "prperror",
        MCA_BASE_MAKE_VERSION(component, ORTE_MAJOR_VERSION, ORTE_MINOR_VERSION,
                ORTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = propagate_prperror_open,
        .mca_close_component = propagate_prperror_close,
        .mca_query_component = propagate_prperror_component_query,
        .mca_register_component_params = propagate_prperror_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int my_priority;

static int propagate_prperror_register(void)
{
    mca_base_component_t *c = &mca_propagate_prperror_component.base_version;

    my_priority = 1000;
    (void) mca_base_component_var_register(c, "priority",
            "Priority of the prperror propagate component",
            MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
            OPAL_INFO_LVL_9,
            MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    return ORTE_SUCCESS;
}

static int propagate_prperror_open(void)
{
    return ORTE_SUCCESS;
}

static int propagate_prperror_close(void)
{
    return ORTE_SUCCESS;
}

static int propagate_prperror_component_query(mca_base_module_t **module, int *priority)
{
    /* only daemon propagate */
    if (ORTE_PROC_IS_DAEMON || ORTE_PROC_IS_HNP ) {
        *priority = my_priority;
        *module = (mca_base_module_t *)&orte_propagate_prperror_module;
        return ORTE_SUCCESS;
    }

    *module = NULL;
    *priority = -1;
    return ORTE_ERROR;
}
