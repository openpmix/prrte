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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/class/prrte_list.h"
#include "src/util/output.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/mca/base/prrte_mca_base_component_repository.h"
#include "constants.h"

void prrte_mca_base_component_unload (const prrte_mca_base_component_t *component, int output_id)
{
    int ret;

    /* Unload */
    prrte_output_verbose (PRRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                         "mca: base: close: unloading component %s",
                         component->mca_component_name);

    ret = prrte_mca_base_var_group_find (component->mca_project_name, component->mca_type_name,
                                   component->mca_component_name);
    if (0 <= ret) {
        prrte_mca_base_var_group_deregister (ret);
    }

    prrte_mca_base_component_repository_release (component);
}

void prrte_mca_base_component_close (const prrte_mca_base_component_t *component, int output_id)
{
    /* Close */
    if (NULL != component->mca_close_component) {
        if( PRRTE_SUCCESS == component->mca_close_component() ) {
            prrte_output_verbose (PRRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                                 "mca: base: close: component %s closed",
                                 component->mca_component_name);
        } else {
            prrte_output_verbose (PRRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                                 "mca: base: close: component %s refused to close [drop it]",
                                 component->mca_component_name);
            return;
        }
    }

    prrte_mca_base_component_unload (component, output_id);
}

int prrte_mca_base_framework_components_close (prrte_mca_base_framework_t *framework,
                                               const prrte_mca_base_component_t *skip)
{
    return prrte_mca_base_components_close (framework->framework_output,
                                            &framework->framework_components,
                                            skip);
}

int prrte_mca_base_components_close(int output_id, prrte_list_t *components,
                                    const prrte_mca_base_component_t *skip)
{
    prrte_mca_base_component_list_item_t *cli, *next;

    /* Close and unload all components in the available list, except the
       "skip" item.  This is handy to close out all non-selected
       components.  It's easier to simply remove the entire list and
       then simply re-add the skip entry when done. */

    PRRTE_LIST_FOREACH_SAFE(cli, next, components, prrte_mca_base_component_list_item_t) {
        if (skip == cli->cli_component) {
            continue;
        }

        prrte_mca_base_component_close (cli->cli_component, output_id);
        prrte_list_remove_item (components, &cli->super);

        PRRTE_RELEASE(cli);
    }

    /* All done */
    return PRRTE_SUCCESS;
}
