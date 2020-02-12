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
#include "src/util/output.h"

#include "src/mca/propagate/propagate.h"
#include "src/mca/propagate/base/base.h"
#include "propagate_prperror.h"

/*
 * Public string for version number
 */
const char *prrte_propagate_prperror_component_version_string =
"PRRTE PROPAGATE prperror MCA component version " PRRTE_VERSION;

/*
 * Local functionality
 */
static int propagate_prperror_register(void);
static int propagate_prperror_open(void);
static int propagate_prperror_close(void);
static int propagate_prperror_component_query(prrte_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
prrte_propagate_base_component_t prrte_propagate_prperror_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component prperror
     */
    .base_version = {
        PRRTE_PROPAGATE_BASE_VERSION_3_0_0,
        /* Component name and version */
        .mca_component_name = "prperror",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                PRRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = propagate_prperror_open,
        .mca_close_component = propagate_prperror_close,
        .mca_query_component = propagate_prperror_component_query,
        .mca_register_component_params = propagate_prperror_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int my_priority;

static int propagate_prperror_register(void)
{
    prrte_mca_base_component_t *c = &prrte_propagate_prperror_component.base_version;

    my_priority = 1000;
    (void) prrte_mca_base_component_var_register(c, "priority",
            "Priority of the prperror propagate component",
            PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
            PRRTE_INFO_LVL_9,
            PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    return PRRTE_SUCCESS;
}

static int propagate_prperror_open(void)
{
    return PRRTE_SUCCESS;
}

static int propagate_prperror_close(void)
{
    return PRRTE_SUCCESS;
}

static int propagate_prperror_component_query(prrte_mca_base_module_t **module, int *priority)
{
    /* only daemon propagate */
    if (PRRTE_PROC_IS_DAEMON || PRRTE_PROC_IS_MASTER ) {
        *priority = my_priority;
        *module = (prrte_mca_base_module_t *)&prrte_propagate_prperror_module;
        return PRRTE_SUCCESS;
    }

    *module = NULL;
    *priority = -1;
    return PRRTE_ERROR;
}
