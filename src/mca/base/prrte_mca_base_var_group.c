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
 * Copyright (c) 2008-2013 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
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
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#include <sys/param.h>
#endif
#include <errno.h>

#include "src/include/prrte_stdint.h"
#include "src/util/show_help.h"
#include "src/mca/mca.h"
#include "src/mca/base/prrte_mca_base_vari.h"
#include "constants.h"
#include "src/util/output.h"
#include "src/util/prrte_environ.h"
#include "src/runtime/runtime.h"

static prrte_pointer_array_t mca_base_var_groups;
static prrte_hash_table_t mca_base_var_group_index_hash;
static int mca_base_var_group_count = 0;
static int mca_base_var_groups_timestamp = 0;
static bool mca_base_var_group_initialized = false;

static void mca_base_var_group_constructor (prrte_mca_base_var_group_t *group);
static void mca_base_var_group_destructor (prrte_mca_base_var_group_t *group);
PRRTE_CLASS_INSTANCE(prrte_mca_base_var_group_t, prrte_object_t,
                   mca_base_var_group_constructor,
                   mca_base_var_group_destructor);

int prrte_mca_base_var_group_init (void)
{
    int ret;

    if (!mca_base_var_group_initialized) {
        PRRTE_CONSTRUCT(&mca_base_var_groups, prrte_pointer_array_t);

        /* These values are arbitrary */
        ret = prrte_pointer_array_init (&mca_base_var_groups, 128, 16384, 128);
        if (PRRTE_SUCCESS != ret) {
            return ret;
        }

        PRRTE_CONSTRUCT(&mca_base_var_group_index_hash, prrte_hash_table_t);
        ret = prrte_hash_table_init (&mca_base_var_group_index_hash, 256);
        if (PRRTE_SUCCESS != ret) {
            return ret;
        }

        mca_base_var_group_initialized = true;
        mca_base_var_group_count = 0;
    }

    return PRRTE_SUCCESS;
}

int prrte_mca_base_var_group_finalize (void)
{
    prrte_object_t *object;
    int size, i;

    if (mca_base_var_group_initialized) {
        size = prrte_pointer_array_get_size(&mca_base_var_groups);
        for (i = 0 ; i < size ; ++i) {
            object = prrte_pointer_array_get_item (&mca_base_var_groups, i);
            if (NULL != object) {
                PRRTE_RELEASE(object);
            }
        }
        PRRTE_DESTRUCT(&mca_base_var_groups);
        PRRTE_DESTRUCT(&mca_base_var_group_index_hash);
        mca_base_var_group_count = 0;
        mca_base_var_group_initialized = false;
    }

    return PRRTE_SUCCESS;
}

int prrte_mca_base_var_group_get_internal (const int group_index, prrte_mca_base_var_group_t **group, bool invalidok)
{
    if (group_index < 0) {
        return PRRTE_ERR_NOT_FOUND;
    }

    *group = (prrte_mca_base_var_group_t *) prrte_pointer_array_get_item (&mca_base_var_groups, group_index);
    if (NULL == *group || (!invalidok && !(*group)->group_isvalid)) {
        *group = NULL;
        return PRRTE_ERR_NOT_FOUND;
    }

    return PRRTE_SUCCESS;
}

static int group_find_by_name (const char *full_name, int *index, bool invalidok)
{
    prrte_mca_base_var_group_t *group;
    void *tmp;
    int rc;

    rc = prrte_hash_table_get_value_ptr (&mca_base_var_group_index_hash, full_name,
                                        strlen (full_name), &tmp);
    if (PRRTE_SUCCESS != rc) {
        return rc;
    }

    rc = prrte_mca_base_var_group_get_internal ((int)(uintptr_t) tmp, &group, invalidok);
    if (PRRTE_SUCCESS != rc) {
        return rc;
    }

    if (invalidok || group->group_isvalid) {
        *index = (int)(uintptr_t) tmp;
        return PRRTE_SUCCESS;
    }

    return PRRTE_ERR_NOT_FOUND;
}

static bool compare_strings (const char *str1, const char *str2) {
    if ((NULL != str1 && 0 == strcmp (str1, "*")) ||
        (NULL == str1 && NULL == str2)) {
        return true;
    }

    if (NULL != str1 && NULL != str2) {
        return 0 == strcmp (str1, str2);
    }

    return false;
}

static int group_find_linear (const char *project_name, const char *framework_name,
                              const char *component_name, bool invalidok)
{
    for (int i = 0 ; i < mca_base_var_group_count ; ++i) {
        prrte_mca_base_var_group_t *group;

        int rc = prrte_mca_base_var_group_get_internal (i, &group, invalidok);
        if (PRRTE_SUCCESS != rc) {
            continue;
        }

        if (compare_strings (project_name, group->group_project) &&
            compare_strings (framework_name, group->group_framework) &&
            compare_strings (component_name, group->group_component)) {
            return i;
        }
    }

    return PRRTE_ERR_NOT_FOUND;
}

static int group_find (const char *project_name, const char *framework_name,
                       const char *component_name, bool invalidok)
{
    char *full_name;
    int ret, index=0;

    if (!prrte_mca_base_var_initialized) {
        return PRRTE_ERR_NOT_FOUND;
    }

    /* check for wildcards */
    if ((project_name && '*' == project_name[0]) || (framework_name && '*' == framework_name[0]) ||
        (component_name && '*' == component_name[0])) {
        return group_find_linear (project_name, framework_name, component_name, invalidok);
    }

    ret = prrte_mca_base_var_generate_full_name4(project_name, framework_name, component_name,
                                                 NULL, &full_name);
    if (PRRTE_SUCCESS != ret) {
        return PRRTE_ERROR;
    }

    ret = group_find_by_name(full_name, &index, invalidok);
    free (full_name);

    return (0 > ret) ? ret : index;
}

static int group_register (const char *project_name, const char *framework_name,
                           const char *component_name, const char *description)
{
    prrte_mca_base_var_group_t *group;
    int group_id, parent_id = -1;
    int ret;

    if (NULL == project_name && NULL == framework_name && NULL == component_name) {
        /* don't create a group with no name (maybe we should create a generic group?) */
        return -1;
    }

    /* avoid groups of the form prrte_prrte */
    if (NULL != project_name && NULL != framework_name &&
        (0 == strcmp (project_name, framework_name))) {
        project_name = NULL;
    }

    group_id = group_find (project_name, framework_name, component_name, true);
    if (0 <= group_id) {
        ret = prrte_mca_base_var_group_get_internal (group_id, &group, true);
        if (PRRTE_SUCCESS != ret) {
            /* something went horribly wrong */
            assert (NULL != group);
            return ret;
        }
        group->group_isvalid = true;
        mca_base_var_groups_timestamp++;

        /* group already exists. return it's index */
        return group_id;
    }

    group = PRRTE_NEW(prrte_mca_base_var_group_t);

    group->group_isvalid = true;

    if (NULL != project_name) {
        group->group_project = strdup (project_name);
        if (NULL == group->group_project) {
            PRRTE_RELEASE(group);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
    }
    if (NULL != framework_name) {
        group->group_framework = strdup (framework_name);
        if (NULL == group->group_framework) {
            PRRTE_RELEASE(group);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
    }
    if (NULL != component_name) {
        group->group_component = strdup (component_name);
        if (NULL == group->group_component) {
            PRRTE_RELEASE(group);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
    }
    if (NULL != description) {
        group->group_description = strdup (description);
        if (NULL == group->group_description) {
            PRRTE_RELEASE(group);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
    }

    if (NULL != framework_name && NULL != component_name) {
        if (component_name) {
            parent_id = group_register (project_name, framework_name, NULL, NULL);
        } else if (framework_name && project_name) {
            parent_id = group_register (project_name, NULL, NULL, NULL);
        }
    }

    /* build the group name */
    ret = prrte_mca_base_var_generate_full_name4 (NULL, project_name, framework_name, component_name,
                                                  &group->group_full_name);
    if (PRRTE_SUCCESS != ret) {
        PRRTE_RELEASE(group);
        return ret;
    }

    group_id = prrte_pointer_array_add (&mca_base_var_groups, group);
    if (0 > group_id) {
        PRRTE_RELEASE(group);
        return PRRTE_ERROR;
    }

    prrte_hash_table_set_value_ptr (&mca_base_var_group_index_hash, group->group_full_name,
                                   strlen (group->group_full_name), (void *)(uintptr_t) group_id);

    mca_base_var_group_count++;
    mca_base_var_groups_timestamp++;

    if (0 <= parent_id) {
        prrte_mca_base_var_group_t *parent_group;

        (void) prrte_mca_base_var_group_get_internal(parent_id, &parent_group, false);
        prrte_value_array_append_item (&parent_group->group_subgroups, &group_id);
    }

    return group_id;
}

int prrte_mca_base_var_group_register (const char *project_name, const char *framework_name,
                                       const char *component_name, const char *description)
{
    return group_register (project_name, framework_name, component_name, description);
}

int prrte_mca_base_var_group_component_register (const prrte_mca_base_component_t *component,
                                                 const char *description)
{
    return group_register (component->mca_project_name, component->mca_type_name,
                           component->mca_component_name, description);
}


int prrte_mca_base_var_group_deregister (int group_index)
{
    prrte_mca_base_var_group_t *group;
    int size, ret;
    int *params, *subgroups;
    prrte_object_t ** enums;

    ret = prrte_mca_base_var_group_get_internal (group_index, &group, false);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    group->group_isvalid = false;

    /* deregister all associated mca parameters */
    size = prrte_value_array_get_size(&group->group_vars);
    params = PRRTE_VALUE_ARRAY_GET_BASE(&group->group_vars, int);

    for (int i = 0 ; i < size ; ++i) {
        const prrte_mca_base_var_t *var;

        ret = prrte_mca_base_var_get (params[i], &var);
        if (PRRTE_SUCCESS != ret || !(var->mbv_flags & PRRTE_MCA_BASE_VAR_FLAG_DWG)) {
            continue;
        }

        (void) prrte_mca_base_var_deregister (params[i]);
    }

    size = prrte_value_array_get_size(&group->group_enums);
    enums = PRRTE_VALUE_ARRAY_GET_BASE(&group->group_enums, prrte_object_t *);
    for (int i = 0 ; i < size ; ++i) {
        PRRTE_RELEASE (enums[i]);
    }

    size = prrte_value_array_get_size(&group->group_subgroups);
    subgroups = PRRTE_VALUE_ARRAY_GET_BASE(&group->group_subgroups, int);
    for (int i = 0 ; i < size ; ++i) {
        (void) prrte_mca_base_var_group_deregister (subgroups[i]);
    }
    /* ordering of variables and subgroups must be the same if the
     * group is re-registered */

    mca_base_var_groups_timestamp++;

    return PRRTE_SUCCESS;
}

int prrte_mca_base_var_group_find (const char *project_name,
                                   const char *framework_name,
                                   const char *component_name)
{
    return group_find (project_name, framework_name, component_name, false);
}

int prrte_mca_base_var_group_find_by_name (const char *full_name, int *index)
{
    return group_find_by_name (full_name, index, false);
}

int prrte_mca_base_var_group_add_var (const int group_index, const int param_index)
{
    prrte_mca_base_var_group_t *group;
    int size, i, ret;
    int *params;

    ret = prrte_mca_base_var_group_get_internal (group_index, &group, false);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    size = prrte_value_array_get_size(&group->group_vars);
    params = PRRTE_VALUE_ARRAY_GET_BASE(&group->group_vars, int);
    for (i = 0 ; i < size ; ++i) {
        if (params[i] == param_index) {
            return i;
        }
    }

    if (PRRTE_SUCCESS !=
        (ret = prrte_value_array_append_item (&group->group_vars, &param_index))) {
        return ret;
    }

    mca_base_var_groups_timestamp++;

    /* return the group index */
    return (int) prrte_value_array_get_size (&group->group_vars) - 1;
}

int prrte_mca_base_var_group_add_enum (const int group_index, const void * storage)
{
    prrte_mca_base_var_group_t *group;
    int size, i, ret;
    void **params;

    ret = prrte_mca_base_var_group_get_internal (group_index, &group, false);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    size = prrte_value_array_get_size(&group->group_enums);
    params = PRRTE_VALUE_ARRAY_GET_BASE(&group->group_enums, void *);
    for (i = 0 ; i < size ; ++i) {
        if (params[i] == storage) {
            return i;
        }
    }

    if (PRRTE_SUCCESS !=
        (ret = prrte_value_array_append_item (&group->group_enums, storage))) {
        return ret;
    }

    /* return the group index */
    return (int) prrte_value_array_get_size (&group->group_enums) - 1;
}

int prrte_mca_base_var_group_get (const int group_index, const prrte_mca_base_var_group_t **group)
{
    return prrte_mca_base_var_group_get_internal (group_index, (prrte_mca_base_var_group_t **) group, false);
}

int prrte_mca_base_var_group_set_var_flag (const int group_index, int flags, bool set)
{
    prrte_mca_base_var_group_t *group;
    int size, i, ret;
    int *vars;

    ret = prrte_mca_base_var_group_get_internal (group_index, &group, false);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    /* set the flag on each valid variable */
    size = prrte_value_array_get_size(&group->group_vars);
    vars = PRRTE_VALUE_ARRAY_GET_BASE(&group->group_vars, int);

    for (i = 0 ; i < size ; ++i) {
        if (0 <= vars[i]) {
            (void) prrte_mca_base_var_set_flag (vars[i], flags, set);
        }
    }

    return PRRTE_SUCCESS;
}


static void mca_base_var_group_constructor (prrte_mca_base_var_group_t *group)
{
    memset ((char *) group + sizeof (group->super), 0, sizeof (*group) - sizeof (group->super));

    PRRTE_CONSTRUCT(&group->group_subgroups, prrte_value_array_t);
    prrte_value_array_init (&group->group_subgroups, sizeof (int));

    PRRTE_CONSTRUCT(&group->group_vars, prrte_value_array_t);
    prrte_value_array_init (&group->group_vars, sizeof (int));

    PRRTE_CONSTRUCT(&group->group_enums, prrte_value_array_t);
    prrte_value_array_init (&group->group_enums, sizeof(void *));
}

static void mca_base_var_group_destructor (prrte_mca_base_var_group_t *group)
{
    free (group->group_full_name);
    group->group_full_name = NULL;

    free (group->group_description);
    group->group_description = NULL;

    free (group->group_project);
    group->group_project = NULL;

    free (group->group_framework);
    group->group_framework = NULL;

    free (group->group_component);
    group->group_component = NULL;

    PRRTE_DESTRUCT(&group->group_subgroups);
    PRRTE_DESTRUCT(&group->group_vars);
    PRRTE_DESTRUCT(&group->group_enums);
}

int prrte_mca_base_var_group_get_count (void)
{
    return mca_base_var_group_count;
}

int prrte_mca_base_var_group_get_stamp (void)
{
    return mca_base_var_groups_timestamp;
}
