/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "src/util/output.h"

#include "src/mca/state/state.h"
#include "src/mca/state/base/base.h"
#include "state_prted.h"

/*
 * Public string for version number
 */
const char *prrte_state_prted_component_version_string =
    "PRRTE STATE prted MCA component version " PRRTE_VERSION;

/*
 * Local functionality
 */
static int state_prted_open(void);
static int state_prted_close(void);
static int state_prted_component_query(prrte_mca_base_module_t **module, int *priority);

/*
 * Instantiate the public struct with all of our public information
 * and pointer to our public functions in it
 */
prrte_state_base_component_t prrte_state_prted_component =
{
    /* Handle the general mca_component_t struct containing
     *  meta information about the component
     */
    .base_version = {
        PRRTE_STATE_BASE_VERSION_1_0_0,
        /* Component name and version */
        .mca_component_name = "prted",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = state_prted_open,
        .mca_close_component = state_prted_close,
        .mca_query_component = state_prted_component_query,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int my_priority=100;

static int state_prted_open(void)
{
    return PRRTE_SUCCESS;
}

static int state_prted_close(void)
{
    return PRRTE_SUCCESS;
}

static int state_prted_component_query(prrte_mca_base_module_t **module, int *priority)
{
    if (PRRTE_PROC_IS_DAEMON) {
        /* set our priority high as we are the default for prteds */
        *priority = my_priority;
        *module = (prrte_mca_base_module_t *)&prrte_state_prted_module;
        return PRRTE_SUCCESS;
    }

    *priority = -1;
    *module = NULL;
    return PRRTE_ERROR;
}
