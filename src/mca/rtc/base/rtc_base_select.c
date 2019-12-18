/*
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/mca.h"
#include "src/mca/base/base.h"

#include "src/mca/rtc/base/base.h"

static bool selected = false;

/*
 * Function for selecting one component from all those that are
 * available.
 */
int prrte_rtc_base_select(void)
{
    prrte_mca_base_component_list_item_t *cli = NULL;
    prrte_mca_base_component_t *component = NULL;
    prrte_mca_base_module_t *module = NULL;
    prrte_rtc_base_module_t *nmodule;
    prrte_rtc_base_selected_module_t *newmodule, *mod;
    int rc, priority;
    bool inserted;

    if (selected) {
        /* ensure we don't do this twice */
        return PRRTE_SUCCESS;
    }
    selected = true;

    /* Query all available components and ask if they have a module */
    PRRTE_LIST_FOREACH(cli, &prrte_rtc_base_framework.framework_components, prrte_mca_base_component_list_item_t) {
        component = (prrte_mca_base_component_t *) cli->cli_component;

        prrte_output_verbose(5, prrte_rtc_base_framework.framework_output,
                            "mca:rtc:select: checking available component %s", component->mca_component_name);

        /* If there's no query function, skip it */
        if (NULL == component->mca_query_component) {
            prrte_output_verbose(5, prrte_rtc_base_framework.framework_output,
                                "mca:rtc:select: Skipping component [%s]. It does not implement a query function",
                                component->mca_component_name );
            continue;
        }

        /* Query the component */
        prrte_output_verbose(5, prrte_rtc_base_framework.framework_output,
                            "mca:rtc:select: Querying component [%s]",
                            component->mca_component_name);
        rc = component->mca_query_component(&module, &priority);

        /* If no module was returned, then skip component */
        if (PRRTE_SUCCESS != rc || NULL == module) {
            prrte_output_verbose(5, prrte_rtc_base_framework.framework_output,
                                "mca:rtc:select: Skipping component [%s]. Query failed to return a module",
                                component->mca_component_name );
            continue;
        }
        nmodule = (prrte_rtc_base_module_t*) module;

        /* give the module a chance to init */
        if (NULL != nmodule->init) {
            if (PRRTE_SUCCESS != (rc = nmodule->init())) {
                prrte_output_verbose(5, prrte_rtc_base_framework.framework_output,
                                    "mca:rtc:select: Skipping component [%s]. Failed to init",
                                    component->mca_component_name );
                continue;
            }
        }

        /* add to the list of selected modules */
        newmodule = PRRTE_NEW(prrte_rtc_base_selected_module_t);
        newmodule->pri = priority;
        newmodule->module = nmodule;
        newmodule->component = component;

        /* maintain priority order */
        inserted = false;
        PRRTE_LIST_FOREACH(mod, &prrte_rtc_base.actives, prrte_rtc_base_selected_module_t) {
            if (priority > mod->pri) {
                prrte_list_insert_pos(&prrte_rtc_base.actives,
                                     (prrte_list_item_t*)mod, &newmodule->super);
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            /* must be lowest priority - add to end */
            prrte_list_append(&prrte_rtc_base.actives, &newmodule->super);
        }
    }

    if (4 < prrte_output_get_verbosity(prrte_rtc_base_framework.framework_output)) {
        prrte_output(0, "%s: Final RTC priorities", PRRTE_NAME_PRINT(PRRTE_PROC_MY_NAME));
        /* show the prioritized list */
        PRRTE_LIST_FOREACH(mod, &prrte_rtc_base.actives, prrte_rtc_base_selected_module_t) {
            prrte_output(0, "\tModule: %s Priority: %d", mod->component->mca_component_name, mod->pri);
        }
    }

    return PRRTE_SUCCESS;
}
