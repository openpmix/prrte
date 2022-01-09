/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2010-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "src/util/output.h"

#include "errmgr_prted.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/errmgr.h"

/*
 * Public string for version number
 */
const char *prte_errmgr_prted_component_version_string
    = "PRTE ERRMGR prted MCA component version " PRTE_VERSION;

/*
 * Local functionality
 */
static int errmgr_prted_register(void);
static int errmgr_prted_open(void);
static int errmgr_prted_close(void);
static int errmgr_prted_component_query(prte_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
prte_errmgr_base_component_t prte_errmgr_prted_component =
{
    /* Handle the general mca_component_t struct containing
     *  meta information about the component itprted
     */
    .base_version = {
        PRTE_ERRMGR_BASE_VERSION_3_0_0,
        /* Component name and version */
        .mca_component_name = "prted",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                    PRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = errmgr_prted_open,
        .mca_close_component = errmgr_prted_close,
        .mca_query_component = errmgr_prted_component_query,
        .mca_register_component_params = errmgr_prted_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT,
        .reserved = {0}
    },
};

static int my_priority;

static int errmgr_prted_register(void)
{
    prte_mca_base_component_t *c = &prte_errmgr_prted_component.base_version;

    my_priority = 1000;
    (void) prte_mca_base_component_var_register(c, "priority",
                                                "Priority of the prted errmgr component",
                                                PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    return PRTE_SUCCESS;
}

static int errmgr_prted_open(void)
{
    return PRTE_SUCCESS;
}

static int errmgr_prted_close(void)
{
    return PRTE_SUCCESS;
}

static int errmgr_prted_component_query(prte_mca_base_module_t **module, int *priority)
{
    if (PRTE_PROC_IS_DAEMON) {
        /* we are the default component for daemons */
        *priority = my_priority;
        *module = (prte_mca_base_module_t *) &prte_errmgr_prted_module;
        return PRTE_SUCCESS;
    }

    *priority = -1;
    *module = NULL;
    return PRTE_ERROR;
}
