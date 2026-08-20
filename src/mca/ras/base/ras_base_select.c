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
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2020      Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * Copyright (c) 2026      Barcelona Supercomputing Center (BSC-CNS).
 *                         All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include "constants.h"

#include <string.h>

#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"

#include "src/mca/ras/base/base.h"

static bool selected = false;

/*
 * Function for selecting one component from all those that are
 * available.
 */
int prte_ras_base_select(void)
{
    pmix_mca_base_component_list_item_t *cli = NULL;
    pmix_mca_base_component_t *component = NULL;
    pmix_mca_base_module_t *module = NULL;
    prte_ras_base_module_t *nmodule;
    prte_ras_base_selected_module_t *newmodule, *mod;
    pmix_list_t candidates;
    int rc, priority;
    bool inserted;

    if (selected) {
        /* ensure we don't do this twice */
        return PRTE_SUCCESS;
    }
    selected = true;

    PMIX_CONSTRUCT(&candidates, pmix_list_t);

    /* Query all available components and ask if they have a module */
    PMIX_LIST_FOREACH(cli, &prte_ras_base_framework.framework_components,
                      pmix_mca_base_component_list_item_t)
    {
        component = (pmix_mca_base_component_t *) cli->cli_component;

        pmix_output_verbose(5, prte_ras_base_framework.framework_output,
                            "mca:ras:select: checking available component %s",
                            component->pmix_mca_component_name);

        /* If there's no query function, skip it */
        if (NULL == component->pmix_mca_query_component) {
            pmix_output_verbose(
                5, prte_ras_base_framework.framework_output,
                "mca:ras:select: Skipping component [%s]. It does not implement a query function",
                component->pmix_mca_component_name);
            continue;
        }

        /* Query the component */
        pmix_output_verbose(5, prte_ras_base_framework.framework_output,
                            "mca:ras:select: Querying component [%s]",
                            component->pmix_mca_component_name);
        rc = component->pmix_mca_query_component(&module, &priority);

        /* If no module was returned, then skip component */
        if (PRTE_SUCCESS != rc || NULL == module) {
            pmix_output_verbose(
                5, prte_ras_base_framework.framework_output,
                "mca:ras:select: Skipping component [%s]. Query failed to return a module",
                component->pmix_mca_component_name);
            continue;
        }

        /* If we got a module, keep it as a CANDIDATE.  Its init() is not run
         * here: only the winner is initialized, and which component wins is
         * not known until every one has been queried.  ras/slurm's init(),
         * for instance, stands up the whole elastic bookkeeping surface. */
        nmodule = (prte_ras_base_module_t *) module;

        /* add to the candidate list */
        newmodule = PMIX_NEW(prte_ras_base_selected_module_t);
        newmodule->pri = priority;
        newmodule->module = nmodule;
        newmodule->component = component;

        /* maintain priority order */
        inserted = false;
        PMIX_LIST_FOREACH(mod, &candidates, prte_ras_base_selected_module_t)
        {
            if (priority > mod->pri) {
                pmix_list_insert_pos(&candidates, (pmix_list_item_t *) mod,
                                     &newmodule->super);
                inserted = true;
                break;
            }
        }
        if (!inserted) {
            /* must be lowest priority - add to end */
            pmix_list_append(&candidates, &newmodule->super);
        }
    }
    if (4 < pmix_output_get_verbosity(prte_ras_base_framework.framework_output)) {
        pmix_output(0, "%s: ras candidates, in priority order", PRTE_NAME_PRINT(PRTE_PROC_MY_NAME));
        PMIX_LIST_FOREACH(mod, &candidates, prte_ras_base_selected_module_t)
        {
            pmix_output(0, "\tComponent: %s Priority: %d", mod->component->pmix_mca_component_name,
                        mod->pri);
        }
    }

    /* Exactly one module is selected: an allocation has exactly one owner.
     *
     * The framework used to keep every module that answered, in priority
     * order, and walk them.  Nothing was ever gained by that and two things
     * were lost.  prte_ras_base_allocate() breaks at the first module that
     * succeeds, so the extra modules never contributed nodes - the mixed
     * allocator this was built for (a scheduler allocation plus a hostfile)
     * was never assembled, because there is no union step and never was.
     * What the extra modules did do was serve prte_ras_base_modify(): a
     * request the real allocator declined fell through to a lower-priority
     * module with no authority over the allocation, and ras/hosts would
     * cheerfully add nodes the scheduler had never granted, after which the
     * daemon launch failed and took the DVM with it.
     *
     * Walk the candidates in priority order and take the first whose init()
     * succeeds.  A failed init is a component that cannot run here, so the
     * next one down is the right answer - that is the only fallback this
     * selection has, and it is a fallback between whole allocators rather
     * than a chain that lets several of them each serve part of a request.
     */
    while (NULL != (mod = (prte_ras_base_selected_module_t *)
                              pmix_list_remove_first(&candidates))) {
        if (NULL != mod->module->init) {
            rc = mod->module->init();
            if (PRTE_SUCCESS != rc) {
                pmix_output_verbose(5, prte_ras_base_framework.framework_output,
                                    "mca:ras:select: Skipping component [%s]. Module init failed",
                                    mod->component->pmix_mca_component_name);
                PMIX_RELEASE(mod);
                continue;
            }
        }
        pmix_list_append(&prte_ras_base.selected_modules, &mod->super);
        break;
    }
    PMIX_LIST_DESTRUCT(&candidates);

    if (pmix_list_is_empty(&prte_ras_base.selected_modules)) {
        /* Not fatal by itself: prte_ras_base_allocate() falls back to the
         * local host when nothing supplies an allocation, which is what a
         * single-node run with no hostfile has always done. */
        pmix_output_verbose(5, prte_ras_base_framework.framework_output,
                            "mca:ras:select: no ras module could be initialized");
        return PRTE_SUCCESS;
    }

    mod = (prte_ras_base_selected_module_t *)
              pmix_list_get_first(&prte_ras_base.selected_modules);
    prte_ras_base.scheduler_owned = mod->module->scheduler_owned;
    pmix_output_verbose(5, prte_ras_base_framework.framework_output,
                        "mca:ras:select: active allocator is [%s] (priority %d, %s)",
                        mod->component->pmix_mca_component_name, mod->pri,
                        prte_ras_base.scheduler_owned ? "scheduler-owned"
                                                      : "locally owned");

    return PRTE_SUCCESS;
}
