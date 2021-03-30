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
 * Copyright (c) 2008-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2011-2015 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "src/class/prte_list.h"
#include "src/mca/base/base.h"
#include "src/mca/base/prte_mca_base_component_repository.h"
#include "src/mca/base/prte_mca_base_framework.h"
#include "src/mca/mca.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/util/show_help.h"

/*
 * Local functions
 */
static int register_components(prte_mca_base_framework_t *framework);
/**
 * Function for finding and opening either all MCA components, or the
 * one that was specifically requested via a MCA parameter.
 */
int prte_mca_base_framework_components_register(prte_mca_base_framework_t *framework,
                                                prte_mca_base_register_flag_t flags)
{
    bool open_dso_components = !(flags & PRTE_MCA_BASE_REGISTER_STATIC_ONLY);
    bool ignore_requested = !!(flags & PRTE_MCA_BASE_REGISTER_ALL);
    int ret;

    /* Find and load requested components */
    ret = prte_mca_base_component_find(NULL, framework, ignore_requested, open_dso_components);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    /* Register all remaining components */
    return register_components(framework);
}

/*
 * Traverse the entire list of found components (a list of
 * prte_mca_base_component_t instances).  If the requested_component_names
 * array is empty, or the name of each component in the list of found
 * components is in the requested_components_array, try to open it.
 * If it opens, add it to the components_available list.
 */
static int register_components(prte_mca_base_framework_t *framework)
{
    int ret;
    prte_mca_base_component_t *component;
    prte_mca_base_component_list_item_t *cli, *next;
    int output_id = framework->framework_output;

    /* Announce */
    prte_output_verbose(PRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                        "mca: base: components_register: registering framework %s components",
                        framework->framework_name);

    /* Traverse the list of found components */

    PRTE_LIST_FOREACH_SAFE(cli, next, &framework->framework_components,
                           prte_mca_base_component_list_item_t)
    {
        component = (prte_mca_base_component_t *) cli->cli_component;

        prte_output_verbose(PRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                            "mca: base: components_register: found loaded component %s",
                            component->mca_component_name);

        /* Call the component's MCA parameter registration function (or open if register doesn't
         * exist) */
        if (NULL == component->mca_register_component_params) {
            prte_output_verbose(PRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                                "mca: base: components_register: "
                                "component %s has no register or open function",
                                component->mca_component_name);
            ret = PRTE_SUCCESS;
        } else {
            ret = component->mca_register_component_params();
        }

        if (PRTE_SUCCESS != ret) {
            if (PRTE_ERR_NOT_AVAILABLE != ret) {
                /* If the component returns PRTE_ERR_NOT_AVAILABLE,
                   it's a cue to "silently ignore me" -- it's not a
                   failure, it's just a way for the component to say
                   "nope!".

                   Otherwise, however, display an error.  We may end
                   up displaying this twice, but it may go to separate
                   streams.  So better to be redundant than to not
                   display the error in the stream where it was
                   expected. */

                if (prte_mca_base_component_show_load_errors) {
                    prte_output_verbose(PRTE_MCA_BASE_VERBOSE_ERROR, output_id,
                                        "mca: base: components_register: component %s "
                                        "/ %s register function failed",
                                        component->mca_type_name, component->mca_component_name);
                }

                prte_output_verbose(PRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                                    "mca: base: components_register: "
                                    "component %s register function failed",
                                    component->mca_component_name);
            }

            prte_list_remove_item(&framework->framework_components, &cli->super);

            /* Release this list item */
            PRTE_RELEASE(cli);
            continue;
        }

        if (NULL != component->mca_register_component_params) {
            prte_output_verbose(PRTE_MCA_BASE_VERBOSE_COMPONENT, output_id,
                                "mca: base: components_register: "
                                "component %s register function successful",
                                component->mca_component_name);
        }

        /* Register this component's version */
        prte_mca_base_component_var_register(component, "major_version", NULL,
                                             PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                             PRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY
                                                 | PRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                             PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
                                             &component->mca_component_major_version);
        prte_mca_base_component_var_register(component, "minor_version", NULL,
                                             PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                             PRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY
                                                 | PRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                             PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
                                             &component->mca_component_minor_version);
        prte_mca_base_component_var_register(component, "release_version", NULL,
                                             PRTE_MCA_BASE_VAR_TYPE_INT, NULL, 0,
                                             PRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY
                                                 | PRTE_MCA_BASE_VAR_FLAG_INTERNAL,
                                             PRTE_INFO_LVL_9, PRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
                                             &component->mca_component_release_version);
    }

    /* All done */

    return PRTE_SUCCESS;
}
