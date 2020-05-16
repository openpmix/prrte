/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2016-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "types.h"
#include "types.h"

#include "src/util/show_help.h"

#include "src/mca/schizo/schizo.h"
#include "schizo_slurm.h"

static int component_query(prte_mca_base_module_t **module, int *priority);

/*
 * Struct of function pointers and all that to let us be initialized
 */
prte_schizo_base_component_t prte_schizo_slurm_component = {
    .base_version = {
        PRTE_MCA_SCHIZO_BASE_VERSION_1_0_0,
        .mca_component_name = "slurm",
        PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                    PRTE_RELEASE_VERSION),
        .mca_query_component = component_query,
    },
    .base_data = {
        /* The component is checkpoint ready */
        PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
    },
};

static int component_query(prte_mca_base_module_t **module, int *priority)
{
    /* disqualify ourselves if we are not under slurm */
    if (NULL == getenv("SLURM_JOBID")) {
        *priority = 0;
        *module = NULL;
        return PRTE_ERROR;
    }

    *module = (prte_mca_base_module_t*)&prte_schizo_slurm_module;
    *priority = 50;
    return PRTE_SUCCESS;
}
