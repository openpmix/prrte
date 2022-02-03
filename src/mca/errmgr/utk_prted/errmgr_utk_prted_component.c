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

#include "errmgr_utk_prted.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"
#include "src/mca/errmgr/errmgr.h"

/*
 * Public string for version number
 */
const char *prte_errmgr_utk_prted_component_version_string
    = "PRTE ERRMGR utk_prted MCA component version " PRTE_VERSION;

/*
 * Local functionality
 */
static int errmgr_utk_prted_register(void);
static int errmgr_utk_prted_open(void);
static int errmgr_utk_prted_close(void);
static int errmgr_utk_prted_component_query(prte_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
prte_errmgr_utk_prted_component_t prte_errmgr_utk_prted_component = {
    .super = {
        /* Handle the general mca_component_t struct containing
         *  meta information about the component utk_prted
         */
        .base_version = {
            PRTE_ERRMGR_BASE_VERSION_3_0_0,
            /* Component name and version */
            .mca_component_name = "utk_prted",
            PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                    PRTE_RELEASE_VERSION),

            /* Component open and close functions */
            .mca_open_component = errmgr_utk_prted_open,
            .mca_close_component = errmgr_utk_prted_close,
            .mca_query_component = errmgr_utk_prted_component_query,
            .mca_register_component_params = errmgr_utk_prted_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    },
    .heartbeat_period = 5.0,
    .heartbeat_timeout = 10.0
};

static int my_priority;

static int errmgr_utk_prted_register(void)
{
    prte_mca_base_component_t *c = &prte_errmgr_utk_prted_component.super.base_version;

    my_priority = 1005;
    (void) prte_mca_base_component_var_register(c, "priority",
                                                "Priority of the utk_prted errmgr component",
                                                PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    (void) prte_mca_base_component_var_register(
        c, "heartbeat_period", "Set heartbeat period for ring utk_prted in errmgr component",
        PRTE_MCA_BASE_VAR_TYPE_DOUBLE, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_errmgr_utk_prted_component.heartbeat_period);

    (void) prte_mca_base_component_var_register(
        c, "heartbeat_timeout", "Set heartbeat timeout for ring utk_prted in errmgr component",
        PRTE_MCA_BASE_VAR_TYPE_DOUBLE, NULL, 0, PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
        PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_errmgr_utk_prted_component.heartbeat_timeout);

    return PRTE_SUCCESS;
}

static int errmgr_utk_prted_open(void)
{
    return PRTE_SUCCESS;
}

static int errmgr_utk_prted_close(void)
{
    return PRTE_SUCCESS;
}

static int errmgr_utk_prted_component_query(prte_mca_base_module_t **module, int *priority)
{
    if (prte_enable_ft && PRTE_PROC_IS_DAEMON) {
        *priority = my_priority;
        *module = (prte_mca_base_module_t *) &prte_errmgr_utk_prted_module;
        return PRTE_SUCCESS;
    }

    *module = NULL;
    *priority = -1;
    return PRTE_ERROR;
}
