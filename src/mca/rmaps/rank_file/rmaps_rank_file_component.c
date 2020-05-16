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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/base/base.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/util/show_help.h"

#include "src/mca/rmaps/base/base.h"
#include "src/mca/rmaps/base/rmaps_private.h"
#include "src/mca/rmaps/rank_file/rmaps_rank_file.h"
#include "src/mca/rmaps/rank_file/rmaps_rank_file_lex.h"

/*
 * Local functions
 */

static int prte_rmaps_rank_file_register(void);
static int prte_rmaps_rank_file_open(void);
static int prte_rmaps_rank_file_close(void);
static int prte_rmaps_rank_file_query(prte_mca_base_module_t **module, int *priority);

static int my_priority;

prte_rmaps_rf_component_t prte_rmaps_rank_file_component = {
    {
        /* First, the prte_mca_base_component_t struct containing meta
           information about the component itself */

        .base_version = {
            PRTE_RMAPS_BASE_VERSION_2_0_0,

            .mca_component_name = "rank_file",
            PRTE_MCA_BASE_MAKE_VERSION(component, PRTE_MAJOR_VERSION, PRTE_MINOR_VERSION,
                                        PRTE_RELEASE_VERSION),
            .mca_open_component = prte_rmaps_rank_file_open,
            .mca_close_component = prte_rmaps_rank_file_close,
            .mca_query_component = prte_rmaps_rank_file_query,
            .mca_register_component_params = prte_rmaps_rank_file_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

/**
  * component register/open/close/init function
  */
static int prte_rmaps_rank_file_register(void)
{
    prte_mca_base_component_t *c = &prte_rmaps_rank_file_component.super.base_version;
    int tmp;

    my_priority = 0;
    (void) prte_mca_base_component_var_register(c, "priority", "Priority of the rank_file rmaps component",
                                           PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           PRTE_INFO_LVL_9,
                                           PRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);
    prte_rankfile = NULL;
    tmp = prte_mca_base_component_var_register(c, "path",
                                          "Name of the rankfile to be used for mapping processes (relative or absolute path)",
                                          PRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                          PRTE_INFO_LVL_5,
                                          PRTE_MCA_BASE_VAR_SCOPE_READONLY, &prte_rankfile);
    (void) prte_mca_base_var_register_synonym(tmp, "prte", "prte", NULL, "rankfile", 0);

    prte_rmaps_rank_file_component.physical = false;
    (void) prte_mca_base_component_var_register(c, "physical", "Rankfile contains physical cpu designations",
                                           PRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                           PRTE_INFO_LVL_5,
                                           PRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                           &prte_rmaps_rank_file_component.physical);


    return PRTE_SUCCESS;
}

static int prte_rmaps_rank_file_open(void)
{
    /* ensure we flag mapping by user */
    if (NULL != prte_rankfile) {
        if (PRTE_MAPPING_GIVEN & PRTE_GET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping)) {
            /* if a non-default mapping is already specified, then we
             * have an error
             */
            prte_show_help("help-prte-rmaps-base.txt", "redefining-policy", true, "mapping",
                           "RANK_FILE", prte_rmaps_base_print_mapping(prte_rmaps_base.mapping));
            PRTE_SET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping, PRTE_MAPPING_CONFLICTED);
            return PRTE_ERR_SILENT;
        }
        PRTE_SET_MAPPING_POLICY(prte_rmaps_base.mapping, PRTE_MAPPING_BYUSER);
        PRTE_SET_MAPPING_DIRECTIVE(prte_rmaps_base.mapping, PRTE_MAPPING_GIVEN);
        /* make us first */
        my_priority = 10000;
    }

    return PRTE_SUCCESS;
}

static int prte_rmaps_rank_file_query(prte_mca_base_module_t **module, int *priority)
{
    *priority = my_priority;
    *module = (prte_mca_base_module_t *)&prte_rmaps_rank_file_module;
    return PRTE_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int prte_rmaps_rank_file_close(void)
{
    int tmp = prte_mca_base_var_find("prte", "prte", NULL, "rankfile");

    if (0 <= tmp) {
        prte_mca_base_var_deregister(tmp);
    }

    return PRTE_SUCCESS;
}

static void rf_map_construct(prte_rmaps_rank_file_map_t *ptr)
{
    ptr->node_name = NULL;
    memset(ptr->slot_list, (char)0x00, 64);
}
static void rf_map_destruct(prte_rmaps_rank_file_map_t *ptr)
{
    if (NULL != ptr->node_name) free(ptr->node_name);
}
PRTE_CLASS_INSTANCE(prte_rmaps_rank_file_map_t,
                   prte_object_t,
                   rf_map_construct,
                   rf_map_destruct);
