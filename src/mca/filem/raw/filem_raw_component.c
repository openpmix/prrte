/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved
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
#include "constants.h"



#include "src/mca/filem/filem.h"
#include "src/mca/filem/base/base.h"
#include "filem_raw.h"

/*
 * Public string for version number
 */
const char *prrte_filem_raw_component_version_string =
"PRRTE FILEM raw MCA component version " PRRTE_VERSION;

/*
 * Local functionality
 */
static int filem_raw_register(void);
static int filem_raw_open(void);
static int filem_raw_close(void);
static int filem_raw_query(prrte_mca_base_module_t **module, int *priority);

bool prrte_filem_raw_flatten_trees=false;

prrte_filem_base_component_t prrte_filem_raw_component = {
    .base_version = {
        PRRTE_FILEM_BASE_VERSION_2_0_0,
        /* Component name and version */
        .mca_component_name = "raw",
        PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                    PRRTE_RELEASE_VERSION),

        /* Component open and close functions */
        .mca_open_component = filem_raw_open,
        .mca_close_component = filem_raw_close,
        .mca_query_component = filem_raw_query,
        .mca_register_component_params = filem_raw_register,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int filem_raw_register(void)
{
    prrte_mca_base_component_t *c = &prrte_filem_raw_component.base_version;

    prrte_filem_raw_flatten_trees = false;
    (void) prrte_mca_base_component_var_register(c, "flatten_directory_trees",
                                           "Put all files in the working directory instead of creating their respective directory trees",
                                           PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                           PRRTE_INFO_LVL_9,
                                           PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                           &prrte_filem_raw_flatten_trees);

    return PRRTE_SUCCESS;
}

static int filem_raw_open(void)
{
    return PRRTE_SUCCESS;
}

static int filem_raw_close(void)
{
    return PRRTE_SUCCESS;
}

static int filem_raw_query(prrte_mca_base_module_t **module, int *priority)
{
    *priority = 0;
    *module = (prrte_mca_base_module_t*) &prrte_filem_raw_module;
    return PRRTE_SUCCESS;
}
