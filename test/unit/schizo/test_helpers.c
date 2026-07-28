/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include <stdarg.h>

#include "test_schizo.h"

/*
 * Build a pmix_cli_item_t the way pmix_cmd_line_parse would: one instance
 * per option key, with every occurrence of that option appended to the same
 * value array.  Reproducing that shape by hand is what lets the sanity
 * checker and the output/display parsers be exercised without running a
 * tool.
 */
void schizo_test_add(pmix_cli_result_t *results, const char *key, ...)
{
    pmix_cli_item_t *opt;
    va_list ap;
    const char *val;

    opt = PMIX_NEW(pmix_cli_item_t);
    opt->key = strdup(key);

    va_start(ap, key);
    while (NULL != (val = va_arg(ap, const char *))) {
        PMIx_Argv_append_nosize(&opt->values, val);
    }
    va_end(ap);

    pmix_list_append(&results->instances, &opt->super);
}

/*
 * Reach a personality through the framework rather than naming its module
 * symbol: the module belongs to its component, which may be a DSO and in
 * any case is not visible outside libprrte in a hidden-visibility build.
 * Returns NULL when the component was not built, so a caller can skip.
 */
prte_schizo_base_module_t *schizo_test_module(const char *name)
{
    prte_schizo_base_active_module_t *mod;

    PMIX_LIST_FOREACH(mod, &prte_schizo_base.active_modules,
                      prte_schizo_base_active_module_t)
    {
        if (NULL != mod->module->name &&
            0 == strcmp(name, mod->module->name)) {
            return mod->module;
        }
    }
    return NULL;
}
