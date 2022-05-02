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
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2022 Nanook Consulting.  All rights reserved.
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
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#include <errno.h>

#include "constants.h"
#include "src/include/prte_stdint.h"
#include "src/mca/base/prte_mca_base_vari.h"
#include "src/mca/mca.h"
#include "src/runtime/runtime.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_show_help.h"

static pmix_pointer_array_t mca_base_var_groups;
static pmix_hash_table_t mca_base_var_group_index_hash;
static int mca_base_var_group_count = 0;
static int mca_base_var_groups_timestamp = 0;
static bool mca_base_var_group_initialized = false;

static void mca_base_var_group_constructor(prte_mca_base_var_group_t *group);
static void mca_base_var_group_destructor(prte_mca_base_var_group_t *group);
PMIX_CLASS_INSTANCE(prte_mca_base_var_group_t, pmix_object_t, mca_base_var_group_constructor,
                    mca_base_var_group_destructor);

int prte_mca_base_var_group_init(void)
{
    int ret;

    if (!mca_base_var_group_initialized) {
        PMIX_CONSTRUCT(&mca_base_var_groups, pmix_pointer_array_t);

        /* These values are arbitrary */
        ret = pmix_pointer_array_init(&mca_base_var_groups, 128, 16384, 128);
        if (PRTE_SUCCESS != ret) {
            return ret;
        }

        PMIX_CONSTRUCT(&mca_base_var_group_index_hash, pmix_hash_table_t);
        ret = pmix_hash_table_init(&mca_base_var_group_index_hash, 256);
        if (PRTE_SUCCESS != ret) {
            return ret;
        }

        mca_base_var_group_initialized = true;
        mca_base_var_group_count = 0;
    }

    return PRTE_SUCCESS;
}

int prte_mca_base_var_group_finalize(void)
{
    pmix_object_t *object;
    int size, i;

    if (mca_base_var_group_initialized) {
        size = pmix_pointer_array_get_size(&mca_base_var_groups);
        for (i = 0; i < size; ++i) {
            object = pmix_pointer_array_get_item(&mca_base_var_groups, i);
            if (NULL != object) {
                PMIX_RELEASE(object);
            }
        }
        PMIX_DESTRUCT(&mca_base_var_groups);
        PMIX_DESTRUCT(&mca_base_var_group_index_hash);
        mca_base_var_group_count = 0;
        mca_base_var_group_initialized = false;
    }

    return PRTE_SUCCESS;
}

int prte_mca_base_var_group_get_internal(const int group_index, prte_mca_base_var_group_t **group,
                                         bool invalidok)
{
    if (group_index < 0) {
        return PRTE_ERR_NOT_FOUND;
    }

    *group = (prte_mca_base_var_group_t *) pmix_pointer_array_get_item(&mca_base_var_groups,
                                                                       group_index);
    if (NULL == *group || (!invalidok && !(*group)->group_isvalid)) {
        *group = NULL;
        return PRTE_ERR_NOT_FOUND;
    }

    return PRTE_SUCCESS;
}

static int group_find_by_name(const char *full_name, int *index, bool invalidok)
{
    prte_mca_base_var_group_t *group;
    void *tmp;
    int rc;

    rc = pmix_hash_table_get_value_ptr(&mca_base_var_group_index_hash, full_name, strlen(full_name),
                                       &tmp);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    rc = prte_mca_base_var_group_get_internal((int) (uintptr_t) tmp, &group, invalidok);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    if (invalidok || group->group_isvalid) {
        *index = (int) (uintptr_t) tmp;
        return PRTE_SUCCESS;
    }

    return PRTE_ERR_NOT_FOUND;
}

static bool compare_strings(const char *str1, const char *str2)
{
    if ((NULL != str1 && 0 == strcmp(str1, "*")) || (NULL == str1 && NULL == str2)) {
        return true;
    }

    if (NULL != str1 && NULL != str2) {
        return 0 == strcmp(str1, str2);
    }

    return false;
}

static int group_find_linear(const char *project_name, const char *framework_name,
                             const char *component_name, bool invalidok)
{
    for (int i = 0; i < mca_base_var_group_count; ++i) {
        prte_mca_base_var_group_t *group;

        int rc = prte_mca_base_var_group_get_internal(i, &group, invalidok);
        if (PRTE_SUCCESS != rc) {
            continue;
        }

        if (compare_strings(project_name, group->group_project)
            && compare_strings(framework_name, group->group_framework)
            && compare_strings(component_name, group->group_component)) {
            return i;
        }
    }

    return PRTE_ERR_NOT_FOUND;
}

static int group_find(const char *project_name, const char *framework_name,
                      const char *component_name, bool invalidok)
{
    char *full_name;
    int ret, index = 0;

    if (!prte_mca_base_var_initialized) {
        return PRTE_ERR_NOT_FOUND;
    }

    /* check for wildcards */
    if ((project_name && '*' == project_name[0]) || (framework_name && '*' == framework_name[0])
        || (component_name && '*' == component_name[0])) {
        return group_find_linear(project_name, framework_name, component_name, invalidok);
    }

    ret = prte_mca_base_var_generate_full_name4(project_name, framework_name, component_name, NULL,
                                                &full_name);
    if (PRTE_SUCCESS != ret) {
        return PRTE_ERROR;
    }

    ret = group_find_by_name(full_name, &index, invalidok);
    free(full_name);

    return (0 > ret) ? ret : index;
}

static int group_register(const char *project_name, const char *framework_name,
                          const char *component_name, const char *description)
{
    prte_mca_base_var_group_t *group;
    int group_id, parent_id = -1;
    int ret;

    if (NULL == project_name && NULL == framework_name && NULL == component_name) {
        /* don't create a group with no name (maybe we should create a generic group?) */
        return -1;
    }

    /* avoid groups of the form prte_prte */
    if (NULL != project_name && NULL != framework_name
        && (0 == strcmp(project_name, framework_name))) {
        project_name = NULL;
    }

    group_id = group_find(project_name, framework_name, component_name, true);
    if (0 <= group_id) {
        ret = prte_mca_base_var_group_get_internal(group_id, &group, true);
        if (PRTE_SUCCESS != ret) {
            /* something went horribly wrong */
            assert(NULL != group);
            return ret;
        }
        group->group_isvalid = true;
        mca_base_var_groups_timestamp++;

        /* group already exists. return it's index */
        return group_id;
    }

    group = PMIX_NEW(prte_mca_base_var_group_t);

    group->group_isvalid = true;

    if (NULL != project_name) {
        group->group_project = strdup(project_name);
        if (NULL == group->group_project) {
            PMIX_RELEASE(group);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
    }
    if (NULL != framework_name) {
        group->group_framework = strdup(framework_name);
        if (NULL == group->group_framework) {
            PMIX_RELEASE(group);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
    }
    if (NULL != component_name) {
        group->group_component = strdup(component_name);
        if (NULL == group->group_component) {
            PMIX_RELEASE(group);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
    }
    if (NULL != description) {
        group->group_description = strdup(description);
        if (NULL == group->group_description) {
            PMIX_RELEASE(group);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }
    }

    if (NULL != framework_name && NULL != component_name) {
        if (component_name) {
            parent_id = group_register(project_name, framework_name, NULL, NULL);
        } else if (framework_name && project_name) {
            parent_id = group_register(project_name, NULL, NULL, NULL);
        }
    }

    /* build the group name */
    ret = prte_mca_base_var_generate_full_name4(NULL, project_name, framework_name, component_name,
                                                &group->group_full_name);
    if (PRTE_SUCCESS != ret) {
        PMIX_RELEASE(group);
        return ret;
    }

    group_id = pmix_pointer_array_add(&mca_base_var_groups, group);
    if (0 > group_id) {
        PMIX_RELEASE(group);
        return PRTE_ERROR;
    }

    pmix_hash_table_set_value_ptr(&mca_base_var_group_index_hash, group->group_full_name,
                                  strlen(group->group_full_name), (void *) (uintptr_t) group_id);

    mca_base_var_group_count++;
    mca_base_var_groups_timestamp++;

    if (0 <= parent_id) {
        prte_mca_base_var_group_t *parent_group;

        (void) prte_mca_base_var_group_get_internal(parent_id, &parent_group, false);
        pmix_value_array_append_item(&parent_group->group_subgroups, &group_id);
    }

    return group_id;
}

int prte_mca_base_var_group_register(const char *project_name, const char *framework_name,
                                     const char *component_name, const char *description)
{
    return group_register(project_name, framework_name, component_name, description);
}

int prte_mca_base_var_group_component_register(const prte_mca_base_component_t *component,
                                               const char *description)
{
    return group_register(component->mca_project_name, component->mca_type_name,
                          component->mca_component_name, description);
}

int prte_mca_base_var_group_deregister(int group_index)
{
    prte_mca_base_var_group_t *group;
    int size, ret;
    int *params, *subgroups;
    pmix_object_t **enums;

    ret = prte_mca_base_var_group_get_internal(group_index, &group, false);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    group->group_isvalid = false;

    /* deregister all associated mca parameters */
    size = pmix_value_array_get_size(&group->group_vars);
    params = PMIX_VALUE_ARRAY_GET_BASE(&group->group_vars, int);

    for (int i = 0; i < size; ++i) {
        const prte_mca_base_var_t *var;

        ret = prte_mca_base_var_get(params[i], &var);
        if (PRTE_SUCCESS != ret || !(var->mbv_flags & PRTE_MCA_BASE_VAR_FLAG_DWG)) {
            continue;
        }

        (void) prte_mca_base_var_deregister(params[i]);
    }

    size = pmix_value_array_get_size(&group->group_enums);
    enums = PMIX_VALUE_ARRAY_GET_BASE(&group->group_enums, pmix_object_t *);
    for (int i = 0; i < size; ++i) {
        PMIX_RELEASE(enums[i]);
    }

    size = pmix_value_array_get_size(&group->group_subgroups);
    subgroups = PMIX_VALUE_ARRAY_GET_BASE(&group->group_subgroups, int);
    for (int i = 0; i < size; ++i) {
        (void) prte_mca_base_var_group_deregister(subgroups[i]);
    }
    /* ordering of variables and subgroups must be the same if the
     * group is re-registered */

    mca_base_var_groups_timestamp++;

    return PRTE_SUCCESS;
}

int prte_mca_base_var_group_find(const char *project_name, const char *framework_name,
                                 const char *component_name)
{
    return group_find(project_name, framework_name, component_name, false);
}

int prte_mca_base_var_group_find_by_name(const char *full_name, int *index)
{
    return group_find_by_name(full_name, index, false);
}

int prte_mca_base_var_group_add_var(const int group_index, const int param_index)
{
    prte_mca_base_var_group_t *group;
    int size, i, ret;
    int *params;

    ret = prte_mca_base_var_group_get_internal(group_index, &group, false);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    size = pmix_value_array_get_size(&group->group_vars);
    params = PMIX_VALUE_ARRAY_GET_BASE(&group->group_vars, int);
    for (i = 0; i < size; ++i) {
        if (params[i] == param_index) {
            return i;
        }
    }

    if (PRTE_SUCCESS != (ret = pmix_value_array_append_item(&group->group_vars, &param_index))) {
        return ret;
    }

    mca_base_var_groups_timestamp++;

    /* return the group index */
    return (int) pmix_value_array_get_size(&group->group_vars) - 1;
}

int prte_mca_base_var_group_add_enum(const int group_index, const void *storage)
{
    prte_mca_base_var_group_t *group;
    int size, i, ret;
    void **params;

    ret = prte_mca_base_var_group_get_internal(group_index, &group, false);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    size = pmix_value_array_get_size(&group->group_enums);
    params = PMIX_VALUE_ARRAY_GET_BASE(&group->group_enums, void *);
    for (i = 0; i < size; ++i) {
        if (params[i] == storage) {
            return i;
        }
    }

    if (PRTE_SUCCESS != (ret = pmix_value_array_append_item(&group->group_enums, storage))) {
        return ret;
    }

    /* return the group index */
    return (int) pmix_value_array_get_size(&group->group_enums) - 1;
}

int prte_mca_base_var_group_get(const int group_index, const prte_mca_base_var_group_t **group)
{
    return prte_mca_base_var_group_get_internal(group_index, (prte_mca_base_var_group_t **) group,
                                                false);
}

int prte_mca_base_var_group_set_var_flag(const int group_index, int flags, bool set)
{
    prte_mca_base_var_group_t *group;
    int size, i, ret;
    int *vars;

    ret = prte_mca_base_var_group_get_internal(group_index, &group, false);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    /* set the flag on each valid variable */
    size = pmix_value_array_get_size(&group->group_vars);
    vars = PMIX_VALUE_ARRAY_GET_BASE(&group->group_vars, int);

    for (i = 0; i < size; ++i) {
        if (0 <= vars[i]) {
            (void) prte_mca_base_var_set_flag(vars[i], flags, set);
        }
    }

    return PRTE_SUCCESS;
}

static void mca_base_var_group_constructor(prte_mca_base_var_group_t *group)
{
    memset((char *) group + sizeof(group->super), 0, sizeof(*group) - sizeof(group->super));

    PMIX_CONSTRUCT(&group->group_subgroups, pmix_value_array_t);
    pmix_value_array_init(&group->group_subgroups, sizeof(int));

    PMIX_CONSTRUCT(&group->group_vars, pmix_value_array_t);
    pmix_value_array_init(&group->group_vars, sizeof(int));

    PMIX_CONSTRUCT(&group->group_enums, pmix_value_array_t);
    pmix_value_array_init(&group->group_enums, sizeof(void *));
}

static void mca_base_var_group_destructor(prte_mca_base_var_group_t *group)
{
    free(group->group_full_name);
    group->group_full_name = NULL;

    free(group->group_description);
    group->group_description = NULL;

    free(group->group_project);
    group->group_project = NULL;

    free(group->group_framework);
    group->group_framework = NULL;

    free(group->group_component);
    group->group_component = NULL;

    PMIX_DESTRUCT(&group->group_subgroups);
    PMIX_DESTRUCT(&group->group_vars);
    PMIX_DESTRUCT(&group->group_enums);
}

int prte_mca_base_var_group_get_count(void)
{
    return mca_base_var_group_count;
}

int prte_mca_base_var_group_get_stamp(void)
{
    return mca_base_var_groups_timestamp;
}
