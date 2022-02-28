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
#include "src/util/output.h"

#include "propagate_prperror.h"
#include "src/mca/propagate/base/base.h"
#include "src/mca/propagate/propagate.h"

/*
 * Public string for version number
 */
const char *prte_propagate_prperror_component_version_string
    = "PRTE PROPAGATE prperror MCA component version " PRTE_VERSION;

/*
 * Local functionality
 */
static int propagate_prperror_register(void);
static int propagate_prperror_open(void);
static int propagate_prperror_close(void);
static int propagate_prperror_component_query(prte_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
prte_propagate_base_component_t prte_propagate_prperror_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component prperror
     */
    .base_version = {
        PRTE_PROPAGATE_BASE_VERSION_3_0_0,
        /* Component name and version */
        .mca_component_name = "prperror",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                PMIX_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = propagate_prperror_open,
        .mca_close_component = propagate_prperror_close,
        .mca_query_component = propagate_prperror_component_query,
        .mca_register_component_params = propagate_prperror_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int my_priority;

static int propagate_prperror_register(void)
{
    prte_mca_base_component_t *c = &prte_propagate_prperror_component.base_version;

    my_priority = 1000;
    (void) prte_mca_base_component_var_register(c, "priority",
                                                "Priority of the prperror propagate component",
                                                PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    return PRTE_SUCCESS;
}

static int propagate_prperror_open(void)
{
    return PRTE_SUCCESS;
}

static int propagate_prperror_close(void)
{
    return PRTE_SUCCESS;
}

static int propagate_prperror_component_query(prte_mca_base_module_t **module, int *priority)
{
    /* only daemon propagate */
    if (prte_enable_ft && (PRTE_PROC_IS_DAEMON || PRTE_PROC_IS_MASTER)) {
        *priority = my_priority;
        *module = (prte_mca_base_module_t *) &prte_propagate_prperror_module;
        return PRTE_SUCCESS;
    }

    *module = NULL;
    *priority = -1;
    return PRTE_ERROR;
}
