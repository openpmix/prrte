/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2004-2008 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2012 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008-2012 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "src/class/prrte_list.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/show_help.h"
#include "src/mca/mca.h"
#include "src/mca/base/base.h"
#include "src/mca/base/prrte_mca_base_framework.h"
#include "src/mca/base/prrte_mca_base_component_repository.h"
#include "constants.h"

/*
 * Local functions
 */
static int register_components(prrte_mca_base_framework_t *framework);
/**
 * Function for finding and opening either all MCA components, or the
 * one that was specifically requested via a MCA parameter.
 */
int prrte_mca_base_framework_components_register (prrte_mca_base_framework_t *framework,
                                                  prrte_mca_base_register_flag_t flags)
{
    bool open_dso_components = !(flags & PRRTE_MCA_BASE_REGISTER_STATIC_ONLY);
    bool ignore_requested = !!(flags & PRRTE_MCA_BASE_REGISTER_ALL);
    int ret;

    /* Find and load requested components */
    ret = prrte_mca_base_component_find(NULL, framework, ignore_requested, open_dso_components);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    /* Register all remaining components */
    return register_components(framework);
}

/*
 * Traverse the entire list of found components (a list of
 * prrte_mca_base_component_t instances).  If the requested_component_names
 * array is empty, or the name of each component in the list of found
 * components is in the requested_components_array, try to open it.
 * If it opens, add it to the components_available list.
 */
static int register_components(prrte_mca_base_framework_t *framework)
{
    int ret;
    prrte_mca_base_component_t *component;
    prrte_mca_base_component_list_item_t *cli, *next;
    int output_id = framework->framework_output;

    /* Announce */
    prrte_output_verbose (PRRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                         "mca: base: components_register: registering framework %s components",
                         framework->framework_name);

    /* Traverse the list of found components */

    PRRTE_LIST_FOREACH_SAFE(cli, next, &framework->framework_components, prrte_mca_base_component_list_item_t) {
        component = (prrte_mca_base_component_t *)cli->cli_component;

        prrte_output_verbose(PRRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                            "mca: base: components_register: found loaded component %s",
                            component->mca_component_name);

        /* Call the component's MCA parameter registration function (or open if register doesn't exist) */
        if (NULL == component->mca_register_component_params) {
            prrte_output_verbose (PRRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                                 "mca: base: components_register: "
                                 "component %s has no register or open function",
                                 component->mca_component_name);
            ret = PRRTE_SUCCESS;
        } else {
            ret = component->mca_register_component_params();
        }

        if (PRRTE_SUCCESS != ret) {
            if (PRRTE_ERR_NOT_AVAILABLE != ret) {
                /* If the component returns PRRTE_ERR_NOT_AVAILABLE,
                   it's a cue to "silently ignore me" -- it's not a
                   failure, it's just a way for the component to say
                   "nope!".

                   Otherwise, however, display an error.  We may end
                   up displaying this twice, but it may go to separate
                   streams.  So better to be redundant than to not
                   display the error in the stream where it was
                   expected. */

                if (prrte_mca_base_component_show_load_errors) {
                    prrte_output_verbose (PRRTE_MCA_BASE_VERBOSE_ERROR, output_id,
                                         "mca: base: components_register: component %s "
                                         "/ %s register function failed",
                                         component->mca_type_name,
                                         component->mca_component_name);
                }

                prrte_output_verbose (PRRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                                     "mca: base: components_register: "
                                     "component %s register function failed",
                                     component->mca_component_name);
            }

            prrte_list_remove_item (&framework->framework_components, &cli->super);

            /* Release this list item */
            PRRTE_RELEASE(cli);
            continue;
        }

        if (NULL != component->mca_register_component_params) {
            prrte_output_verbose (PRRTE_MCA_BASE_VERBOSE_COMPONENT, output_id, "mca: base: components_register: "
                                 "component %s register function successful",
                                 component->mca_component_name);
        }

        /* Register this component's version */
        prrte_mca_base_component_var_register (component, "major_version", NULL, PRRTE_MCA_BASE_VAR_TYPE_INT, NULL,
                                               0, PRRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY | PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                               PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
                                               &component->mca_component_major_version);
        prrte_mca_base_component_var_register (component, "minor_version", NULL, PRRTE_MCA_BASE_VAR_TYPE_INT, NULL,
                                               0, PRRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY | PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                               PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
                                               &component->mca_component_minor_version);
        prrte_mca_base_component_var_register (component, "release_version", NULL, PRRTE_MCA_BASE_VAR_TYPE_INT, NULL,
                                               0, PRRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY | PRRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                               PRRTE_INFO_LVL_9, PRRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
                                               &component->mca_component_release_version);
    }

    /* All done */

    return PRRTE_SUCCESS;
}
