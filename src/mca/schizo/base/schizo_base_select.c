/*
 * Copyright (c) 2015-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <stdio.h>
#include <string.h>

#include "src/mca/mca.h"
#include "src/util/output.h"
#include "src/mca/base/base.h"

#include "src/util/show_help.h"

#include "src/runtime/prrte_globals.h"
#include "src/mca/schizo/schizo.h"
#include "src/mca/schizo/base/base.h"

/**
 * Function for selecting all runnable modules from those that are
 * available.
 */

int prrte_schizo_base_select(void)
{
    prrte_mca_base_component_list_item_t *cli = NULL;
    prrte_mca_base_component_t *component = NULL;
    prrte_mca_base_module_t *module = NULL;
    prrte_schizo_base_module_t *nmodule;
    prrte_schizo_base_active_module_t *newmodule, *mod;
    int rc, priority;
    bool inserted;

    if (0 < prrte_list_get_size(&prrte_schizo_base.active_modules)) {
        /* ensure we don't do this twice */
        return PRRTE_SUCCESS;
    }

    /* Query all available components and ask if they have a module */
    PRRTE_LIST_FOREACH(cli, &prrte_schizo_base_framework.framework_components, prrte_mca_base_component_list_item_t) {
        component = (prrte_mca_base_component_t *) cli->cli_component;

        prrte_output_verbose(5, prrte_schizo_base_framework.framework_output,
                            "mca:schizo:select: checking available component %s", component->mca_component_name);

        /* If there's no query function, skip it */
        if (NULL == component->mca_query_component) {
            prrte_output_verbose(5, prrte_schizo_base_framework.framework_output,
                                "mca:schizo:select: Skipping component [%s]. It does not implement a query function",
                                component->mca_component_name );
            continue;
        }

        /* Query the component */
        prrte_output_verbose(5, prrte_schizo_base_framework.framework_output,
                            "mca:schizo:select: Querying component [%s]",
                            component->mca_component_name);
        rc = component->mca_query_component(&module, &priority);

        /* If no module was returned, then skip component */
        if (PRRTE_SUCCESS != rc || NULL == module) {
            prrte_output_verbose(5, prrte_schizo_base_framework.framework_output,
                                "mca:schizo:select: Skipping component [%s]. Query failed to return a module",
                                component->mca_component_name );
            continue;
        }

        /* If we got a module, keep it */
        nmodule = (prrte_schizo_base_module_t*) module;
        /* add to the list of active modules */
        newmodule = PRRTE_NEW(prrte_schizo_base_active_module_t);
        newmodule->pri = priority;
        newmodule->module = nmodule;
        newmodule->component = component;

        /* maintain priority order */
        inserted = false;
        PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
            if (priority > mod->pri) {
                prrte_list_insert_pos(&prrte_schizo_base.active_modules,
                                     (prrte_list_item_t*)mod, &newmodule->super);
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            /* must be lowest priority - add to end */
            prrte_list_append(&prrte_schizo_base.active_modules, &newmodule->super);
        }
    }

    if (4 < prrte_output_get_verbosity(prrte_schizo_base_framework.framework_output)) {
        prrte_output(0, "Final schizo priorities");
        /* show the prioritized list */
        PRRTE_LIST_FOREACH(mod, &prrte_schizo_base.active_modules, prrte_schizo_base_active_module_t) {
            prrte_output(0, "\tSchizo: %s Priority: %d", mod->component->mca_component_name, mod->pri);
        }
    }

    return PRRTE_SUCCESS;;
}
