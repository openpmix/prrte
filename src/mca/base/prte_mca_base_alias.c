/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2020      Google, LLC. All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"
#include <string.h>

#include "src/class/pmix_hash_table.h"
#include "src/include/constants.h"
#include "src/mca/base/prte_mca_base_alias.h"

static void prte_mca_base_alias_init(prte_mca_base_alias_t *alias)
{
    PMIX_CONSTRUCT(&alias->component_aliases, pmix_list_t);
}

static void prte_mca_base_alias_fini(prte_mca_base_alias_t *alias)
{
    PMIX_LIST_DESTRUCT(&alias->component_aliases);
}

PMIX_CLASS_INSTANCE(prte_mca_base_alias_t, pmix_object_t, prte_mca_base_alias_init,
                    prte_mca_base_alias_fini);

static void prte_mca_base_alias_item_init(prte_mca_base_alias_item_t *alias_item)
{
    alias_item->component_alias = NULL;
}

static void prte_mca_base_alias_item_fini(prte_mca_base_alias_item_t *alias_item)
{
    free(alias_item->component_alias);
}

PMIX_CLASS_INSTANCE(prte_mca_base_alias_item_t, pmix_list_item_t, prte_mca_base_alias_item_init,
                    prte_mca_base_alias_item_fini);

/*
 * local variables
 */
static pmix_hash_table_t *alias_hash_table;

void prte_mca_base_alias_cleanup(void)
{
    if (!alias_hash_table) {
        return;
    }

    void *key;
    pmix_object_t *value;
    PMIX_HASH_TABLE_FOREACH_PTR(key, value, alias_hash_table, { PMIX_RELEASE(value); });

    PMIX_RELEASE(alias_hash_table);
    alias_hash_table = NULL;
}

static int prte_mca_base_alias_setup(void)
{
    if (NULL != alias_hash_table) {
        return PRTE_SUCCESS;
    }

    alias_hash_table = PMIX_NEW(pmix_hash_table_t);
    if (NULL == alias_hash_table) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    int ret = pmix_hash_table_init(alias_hash_table, 32);
    if (PRTE_SUCCESS != ret) {
        PMIX_RELEASE(alias_hash_table);
        alias_hash_table = NULL;
        return ret;
    }

    return PRTE_SUCCESS;
}

static char *prte_mca_base_alias_generate_name(const char *project, const char *framework,
                                               const char *component_name)
{
    size_t project_length = project ? strlen(project) : 0;
    size_t framework_length = framework ? strlen(framework) : 0;
    size_t component_name_length = strlen(component_name);
    size_t length = project_length + framework_length + component_name_length + 2;
    char *tmp = calloc(1, length + 1);
    if (NULL == tmp) {
        return tmp;
    }

    if (project_length) {
        strncat(tmp, project, length);
        // NOTE: We use strcat() here (and not strncat()) because
        // we're appending a constant string with a fixed/hard-coded
        // length.  It's the exact equivalent of strncat(tmp, "_", 1),
        // but strncat() would actually be more overhead.  Indeed, GCC
        // 10 emits a warning if we use strncatd() with a compile-time
        // constant string as the source and a hard-coded length that
        // is equivalent to the length of that compile-time constant
        // string.  So avoid the warning and use strcat().
        strcat(tmp, "_");
        length -= project_length + 1;
    }

    if (framework_length) {
        strncat(tmp, framework, length);
        // Use strcat() here instead of strncat(); see the comment
        // above for an explanation.
        strcat(tmp, "_");
        length -= framework_length + 1;
    }

    strncat(tmp, component_name, length);

    return tmp;
}

static prte_mca_base_alias_t *prte_mca_base_alias_lookup_internal(const char *name)
{
    prte_mca_base_alias_t *alias = NULL;
    if (NULL == alias_hash_table) {
        return NULL;
    }

    (void) pmix_hash_table_get_value_ptr(alias_hash_table, name, strlen(name), (void **) &alias);
    return alias;
}

int prte_mca_base_alias_register(const char *project, const char *framework,
                                 const char *component_name, const char *component_alias,
                                 uint32_t alias_flags)
{
    if (NULL == component_name) {
        return PRTE_ERR_BAD_PARAM;
    }

    int ret = prte_mca_base_alias_setup();
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    char *name = prte_mca_base_alias_generate_name(project, framework, component_name);
    assert(NULL != name);

    prte_mca_base_alias_t *alias = prte_mca_base_alias_lookup_internal(name);
    if (NULL == alias) {
        alias = PMIX_NEW(prte_mca_base_alias_t);
        if (NULL == alias) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        pmix_hash_table_set_value_ptr(alias_hash_table, name, strlen(name), alias);
        free(name);
        name = NULL;
    }

    prte_mca_base_alias_item_t *alias_item = PMIX_NEW(prte_mca_base_alias_item_t);
    if (NULL == alias_item) {
        if (NULL != name)
            free(name);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    alias_item->component_alias = strdup(component_alias);
    alias_item->alias_flags = alias_flags;

    pmix_list_append(&alias->component_aliases, &alias_item->super);

    return PRTE_SUCCESS;
}

const prte_mca_base_alias_t *prte_mca_base_alias_lookup(const char *project, const char *framework,
                                                        const char *component_name)
{
    if (NULL == component_name) {
        return NULL;
    }

    char *name = prte_mca_base_alias_generate_name(project, framework, component_name);
    assert(NULL != name);
    const prte_mca_base_alias_t *alias = prte_mca_base_alias_lookup_internal(name);
    free(name);

    return alias;
}
