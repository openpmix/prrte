/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Voltaire. All rights reserved
 * Copyright (c) 2011-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/hwloc/hwloc-internal.h"
#include "src/mca/base/base.h"

#include "src/util/pmix_show_help.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/rank_file/rmaps_rank_file.h"
#include "src/mca/rmaps/rank_file/rmaps_rank_file_lex.h"

/*
 * Local functions
 */

static int prte_rmaps_rank_file_query(prte_mca_base_module_t **module, int *priority);

prte_rmaps_rf_component_t prte_rmaps_rank_file_component = {
    {
        /* First, the prte_mca_base_component_t struct containing meta
           information about the component itself */

        .base_version = {
            PRTE_RMAPS_BASE_VERSION_2_0_0,

            .mca_component_name = "rank_file",
            PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                        PMIX_RELEASE_VERSION),
            .mca_query_component = prte_rmaps_rank_file_query,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

static int prte_rmaps_rank_file_query(prte_mca_base_module_t **module, int *priority)
{
    *priority = 0;
    *module = (prte_mca_base_module_t *) &prte_rmaps_rank_file_module;
    return PRTE_SUCCESS;
}

static void rf_map_construct(prte_rmaps_rank_file_map_t *ptr)
{
    ptr->node_name = NULL;
    memset(ptr->slot_list, (char) 0x00, 64);
}
static void rf_map_destruct(prte_rmaps_rank_file_map_t *ptr)
{
    if (NULL != ptr->node_name)
        free(ptr->node_name);
}
PMIX_CLASS_INSTANCE(prte_rmaps_rank_file_map_t, pmix_object_t, rf_map_construct, rf_map_destruct);
