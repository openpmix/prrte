/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"
#include "src/util/output.h"

#include "filem_raw.h"
#include "src/mca/filem/base/base.h"
#include "src/mca/filem/filem.h"

/*
 * Public string for version number
 */
const char *prte_filem_raw_component_version_string
    = "PRTE FILEM raw MCA component version " PRTE_VERSION;

/*
 * Local functionality
 */
static int filem_raw_register(void);
static int filem_raw_open(void);
static int filem_raw_close(void);
static int filem_raw_query(prte_mca_base_module_t **module, int *priority);

bool prte_filem_raw_flatten_trees = false;

prte_filem_base_component_t prte_filem_raw_component = {
    .base_version = {
        PRTE_FILEM_BASE_VERSION_2_0_0,
        /* Component name and version */
        .mca_component_name = "raw",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                    PRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = filem_raw_open,
        .mca_close_component = filem_raw_close,
        .mca_query_component = filem_raw_query,
        .mca_register_component_params = filem_raw_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int filem_raw_register(void)
{
    prte_mca_base_component_t *c = &prte_filem_raw_component.base_version;

    prte_filem_raw_flatten_trees = false;
    (void) prte_mca_base_component_var_register(c, "flatten_directory_trees",
                                                "Put all files in the working directory instead of "
                                                "creating their respective directory trees",
                                                PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0,
                                                PRTE_MCA_BASE_VAR_FLAG_NONE, PRTE_INFO_LVL_9,
                                                PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                                &prte_filem_raw_flatten_trees);

    return PRTE_SUCCESS;
}

static int filem_raw_open(void)
{
    return PRTE_SUCCESS;
}

static int filem_raw_close(void)
{
    return PRTE_SUCCESS;
}

static int filem_raw_query(prte_mca_base_module_t **module, int *priority)
{
    *priority = 0;
    *module = (prte_mca_base_module_t *) &prte_filem_raw_module;
    return PRTE_SUCCESS;
}
