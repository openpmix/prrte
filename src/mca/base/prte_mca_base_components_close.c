/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2006 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2006 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2006 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2013-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
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
#include "src/class/prte_list.h"
#include "src/mca/base/base.h"
#include "src/mca/base/prte_mca_base_component_repository.h"
#include "src/mca/mca.h"
#include "src/util/output.h"

void prte_mca_base_component_unload(const prte_mca_base_component_t *component, int output_id)
{
    int ret;

    /* Unload */
    prte_output_verbose(PRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                        "mca: base: close: unloading component %s", component->mca_component_name);

    ret = prte_mca_base_var_group_find(component->mca_project_name, component->mca_type_name,
                                       component->mca_component_name);
    if (0 <= ret) {
        prte_mca_base_var_group_deregister(ret);
    }

    prte_mca_base_component_repository_release(component);
}

void prte_mca_base_component_close(const prte_mca_base_component_t *component, int output_id)
{
    /* Close */
    if (NULL != component->mca_close_component) {
        if (PRTE_SUCCESS == component->mca_close_component()) {
            prte_output_verbose(PRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                                "mca: base: close: component %s closed",
                                component->mca_component_name);
        } else {
            prte_output_verbose(PRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                                "mca: base: close: component %s refused to close [drop it]",
                                component->mca_component_name);
            return;
        }
    }

    prte_mca_base_component_unload(component, output_id);
}

int prte_mca_base_framework_components_close(prte_mca_base_framework_t *framework,
                                             const prte_mca_base_component_t *skip)
{
    return prte_mca_base_components_close(framework->framework_output,
                                          &framework->framework_components, skip);
}

int prte_mca_base_components_close(int output_id, prte_list_t *components,
                                   const prte_mca_base_component_t *skip)
{
    prte_mca_base_component_list_item_t *cli, *next;

    /* Close and unload all components in the available list, except the
       "skip" item.  This is handy to close out all non-selected
       components.  It's easier to simply remove the entire list and
       then simply re-add the skip entry when done. */

    PRTE_LIST_FOREACH_SAFE(cli, next, components, prte_mca_base_component_list_item_t)
    {
        if (skip == cli->cli_component) {
            continue;
        }

        prte_mca_base_component_close(cli->cli_component, output_id);
        prte_list_remove_item(components, &cli->super);

        PRTE_RELEASE(cli);
    }

    /* All done */
    return PRTE_SUCCESS;
}
