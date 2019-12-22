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
 * Copyright (c) 2011      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
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

#include "prrte_config.h"
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

static int prrte_rmaps_rank_file_register(void);
static int prrte_rmaps_rank_file_open(void);
static int prrte_rmaps_rank_file_close(void);
static int prrte_rmaps_rank_file_query(prrte_mca_base_module_t **module, int *priority);

static int my_priority;

prrte_rmaps_rf_component_t prrte_rmaps_rank_file_component = {
    {
        /* First, the prrte_mca_base_component_t struct containing meta
           information about the component itself */

        .base_version = {
            PRRTE_RMAPS_BASE_VERSION_2_0_0,

            .mca_component_name = "rank_file",
            PRRTE_MCA_BASE_MAKE_VERSION(component, PRRTE_MAJOR_VERSION, PRRTE_MINOR_VERSION,
                                        PRRTE_RELEASE_VERSION),
            .mca_open_component = prrte_rmaps_rank_file_open,
            .mca_close_component = prrte_rmaps_rank_file_close,
            .mca_query_component = prrte_rmaps_rank_file_query,
            .mca_register_component_params = prrte_rmaps_rank_file_register,
        },
        .base_data = {
            /* The component is checkpoint ready */
            PRRTE_MCA_BASE_METADATA_PARAM_CHECKPOINT
        },
    }
};

/**
  * component register/open/close/init function
  */
static int prrte_rmaps_rank_file_register(void)
{
    prrte_mca_base_component_t *c = &prrte_rmaps_rank_file_component.super.base_version;
    int tmp;

    my_priority = 0;
    (void) prrte_mca_base_component_var_register(c, "priority", "Priority of the rank_file rmaps component",
                                           PRRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0, 0,
                                           PRRTE_INFO_LVL_9,
                                           PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &my_priority);
    prrte_rankfile = NULL;
    tmp = prrte_mca_base_component_var_register(c, "path",
                                          "Name of the rankfile to be used for mapping processes (relative or absolute path)",
                                          PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0,
                                          PRRTE_INFO_LVL_5,
                                          PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_rankfile);
    (void) prrte_mca_base_var_register_synonym(tmp, "prrte", "prrte", NULL, "rankfile", 0);

    prrte_rmaps_rank_file_component.physical = false;
    (void) prrte_mca_base_component_var_register(c, "physical", "Rankfile contains physical cpu designations",
                                           PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0,
                                           PRRTE_INFO_LVL_5,
                                           PRRTE_MCA_BASE_VAR_SCOPE_READONLY,
                                           &prrte_rmaps_rank_file_component.physical);


    return PRRTE_SUCCESS;
}

static int prrte_rmaps_rank_file_open(void)
{
    /* ensure we flag mapping by user */
    if ((PRRTE_BIND_TO_CPUSET == PRRTE_GET_BINDING_POLICY(prrte_hwloc_binding_policy) &&
        !PRRTE_BIND_ORDERED_REQUESTED(prrte_hwloc_binding_policy)) ||
         NULL != prrte_rankfile) {
        if (PRRTE_MAPPING_GIVEN & PRRTE_GET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping)) {
            /* if a non-default mapping is already specified, then we
             * have an error
             */
            prrte_show_help("help-prrte-rmaps-base.txt", "redefining-policy", true, "mapping",
                           "RANK_FILE", prrte_rmaps_base_print_mapping(prrte_rmaps_base.mapping));
            PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_CONFLICTED);
            return PRRTE_ERR_SILENT;
        }
        PRRTE_SET_MAPPING_POLICY(prrte_rmaps_base.mapping, PRRTE_MAPPING_BYUSER);
        PRRTE_SET_MAPPING_DIRECTIVE(prrte_rmaps_base.mapping, PRRTE_MAPPING_GIVEN);
        /* we are going to bind to cpuset since the user is specifying the cpus */
        PRRTE_SET_BINDING_POLICY(prrte_hwloc_binding_policy, PRRTE_BIND_TO_CPUSET);
        /* make us first */
        my_priority = 10000;
    }

    return PRRTE_SUCCESS;
}

static int prrte_rmaps_rank_file_query(prrte_mca_base_module_t **module, int *priority)
{
    *priority = my_priority;
    *module = (prrte_mca_base_module_t *)&prrte_rmaps_rank_file_module;
    return PRRTE_SUCCESS;
}

/**
 *  Close all subsystems.
 */

static int prrte_rmaps_rank_file_close(void)
{
    int tmp = prrte_mca_base_var_find("prrte", "prrte", NULL, "rankfile");

    if (0 <= tmp) {
        prrte_mca_base_var_deregister(tmp);
    }

    return PRRTE_SUCCESS;
}

static void rf_map_construct(prrte_rmaps_rank_file_map_t *ptr)
{
    ptr->node_name = NULL;
    memset(ptr->slot_list, (char)0x00, 64);
}
static void rf_map_destruct(prrte_rmaps_rank_file_map_t *ptr)
{
    if (NULL != ptr->node_name) free(ptr->node_name);
}
PRRTE_CLASS_INSTANCE(prrte_rmaps_rank_file_map_t,
                   prrte_object_t,
                   rf_map_construct,
                   rf_map_destruct);
