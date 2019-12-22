/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2010      Cisco Systems, Inc. All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "src/util/output.h"

#include "src/mca/errmgr/errmgr.h"
#include "src/mca/errmgr/base/base.h"
#include "src/mca/errmgr/base/errmgr_private.h"
#include "errmgr_dvm.h"

/*
 * Public string for version number
 */
const char *prrte_errmgr_dvm_component_version_string =
    "PRRTE ERRMGR dvm MCA component version " PRRTE_VERSION;

/*
 * Local functionality
 */
static int dvm_register(void);
static int dvm_open(void);
static int dvm_close(void);
static int dvm_component_query(prrte_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
prrte_errmgr_base_component_t prrte_errmgr_dvm_component = {
    /* Handle the general mca_component_t struct containing
     *  meta information about the component dvm
     */
    .base_version = {
        PRRTE_ERRMGR_BASE_VERSION_3_0_0,
        /* Component name and version */
        .mca_component_name = "dvm",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = dvm_open,
        .mca_close_component = dvm_close,
        .mca_query_component = dvm_component_query,
        .mca_register_component_params = dvm_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int my_priority;

static int dvm_register(void)
{
    prrte_mca_base_component_t *c = &prrte_errmgr_dvm_component.base_version;

    my_priority = 1000;
    (void) prrte_mca_base_component_var_register(c, "priority",
                                           "Priority of the dvm errmgr component",
                                           PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           PRRTE_INFO_LVL_9,
                                           PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);

    return PRRTE_SUCCESS;
}

static int dvm_open(void)
{
    return PRRTE_SUCCESS;
}

static int dvm_close(void)
{
    return PRRTE_SUCCESS;
}

static int dvm_component_query(prrte_mca_base_module_t **module, int *priority)
{
    /* used by DVM masters */
    if (PRRTE_PROC_IS_MASTER) {
        *priority = my_priority;
        *module = (prrte_mca_base_module_t *)&prrte_errmgr_dvm_module;
        return PRRTE_SUCCESS;
    }

    *module = NULL;
    *priority = -1;
    return PRRTE_ERROR;
}
