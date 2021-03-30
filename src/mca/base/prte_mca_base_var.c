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
 * Copyright (c) 2012-2018 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
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
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
#    include <sys/param.h>
#endif
#include <errno.h>

#include "constants.h"
#include "src/include/prte_stdint.h"
#include "src/mca/base/prte_mca_base_alias.h"
#include "src/mca/base/prte_mca_base_vari.h"
#include "src/mca/mca.h"
#include "src/mca/prteinstalldirs/prteinstalldirs.h"
#include "src/runtime/runtime.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/keyval_parse.h"
#include "src/util/os_path.h"
#include "src/util/output.h"
#include "src/util/path.h"
#include "src/util/printf.h"
#include "src/util/prte_environ.h"
#include "src/util/show_help.h"

/*
 * local variables
 */
static prte_pointer_array_t prte_mca_base_vars;
static const char *prte_mca_prefix = PRTE_MCA_PREFIX;
static char *home = NULL;
bool prte_mca_base_var_initialized = false;
static char *prte_mca_base_envar_files = NULL;
static char **prte_mca_base_var_file_list = NULL;
static bool prte_mca_base_var_suppress_override_warning = false;
static prte_list_t prte_mca_base_var_file_values;
static prte_list_t prte_mca_base_envar_file_values;

static int prte_mca_base_var_count = 0;

static prte_hash_table_t prte_mca_base_var_index_hash;

#define PRTE_MCA_VAR_MBV_ENUMERATOR_FREE(mbv_enumerator)         \
    {                                                            \
        if (mbv_enumerator && !mbv_enumerator->enum_is_static) { \
            PRTE_RELEASE(mbv_enumerator);                        \
        }                                                        \
    }

const char *prte_var_type_names[] = {
    "int",     "unsigned_int", "unsigned_long",  "unsigned_long_long",
    "size_t",  "string",       "version_string", "bool",
    "double",  "long",         "int32_t",        "uint32_t",
    "int64_t", "uint64_t",
};

const size_t prte_var_type_sizes[] = {
    sizeof(int),     sizeof(unsigned), sizeof(unsigned long), sizeof(unsigned long long),
    sizeof(size_t),  sizeof(char),     sizeof(char),          sizeof(bool),
    sizeof(double),  sizeof(long),     sizeof(int32_t),       sizeof(uint32_t),
    sizeof(int64_t), sizeof(uint64_t),
};

static const char *prte_var_source_names[] = {"default", "command line", "environment",
                                              "file",    "set",          "override"};

static const char *prte_info_lvl_strings[] = {"user/basic",  "user/detail",  "user/all",
                                              "tuner/basic", "tuner/detail", "tuner/all",
                                              "dev/basic",   "dev/detail",   "dev/all"};

/*
 * local functions
 */
static int var_set_initial(prte_mca_base_var_t *var, prte_mca_base_var_t *original);
static int var_get(int vari, prte_mca_base_var_t **var_out, bool original);
static int var_value_string(prte_mca_base_var_t *var, char **value_string);

/*
 * classes
 */
static void var_constructor(prte_mca_base_var_t *p);
static void var_destructor(prte_mca_base_var_t *p);
PRTE_CLASS_INSTANCE(prte_mca_base_var_t, prte_object_t, var_constructor, var_destructor);

static void fv_constructor(prte_mca_base_var_file_value_t *p);
static void fv_destructor(prte_mca_base_var_file_value_t *p);
PRTE_CLASS_INSTANCE(prte_mca_base_var_file_value_t, prte_list_item_t, fv_constructor,
                    fv_destructor);

static const char *prte_mca_base_var_source_file(const prte_mca_base_var_t *var)
{
    prte_mca_base_var_file_value_t *fv = (prte_mca_base_var_file_value_t *) var->mbv_file_value;

    if (NULL != var->mbv_source_file) {
        return var->mbv_source_file;
    }

    if (fv) {
        return fv->mbvfv_file;
    }

    return NULL;
}

/*
 * Generate a full name from three names
 */
int prte_mca_base_var_generate_full_name4(const char *project, const char *framework,
                                          const char *component, const char *variable,
                                          char **full_name)
{
    const char *const names[] = {project, framework, component, variable};
    char *name, *tmp;
    size_t i, len;

    *full_name = NULL;

    for (i = 0, len = 0; i < 4; ++i) {
        if (NULL != names[i]) {
            /* Add space for the string + _ or \0 */
            len += strlen(names[i]) + 1;
        }
    }

    name = calloc(1, len);
    if (NULL == name) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0, tmp = name; i < 4; ++i) {
        if (NULL != names[i]) {
            if (name != tmp) {
                *tmp++ = '_';
            }
            strncat(name, names[i], len - (size_t)(uintptr_t)(tmp - name));
            tmp += strlen(names[i]);
        }
    }

    *full_name = name;
    return PRTE_SUCCESS;
}

static int compare_strings(const char *str1, const char *str2)
{
    if ((NULL != str1 && 0 == strcmp(str1, "*")) || (NULL == str1 && NULL == str2)) {
        return 0;
    }

    if (NULL != str1 && NULL != str2) {
        return strcmp(str1, str2);
    }

    return 1;
}

/*
 * Append a filename to the file list if it does not exist and return a
 * pointer to the filename in the list.
 */
static char *append_filename_to_list(const char *filename)
{
    int i, count;

    (void) prte_argv_append_unique_nosize(&prte_mca_base_var_file_list, filename);

    count = prte_argv_count(prte_mca_base_var_file_list);

    for (i = count - 1; i >= 0; --i) {
        if (0 == strcmp(prte_mca_base_var_file_list[i], filename)) {
            return prte_mca_base_var_file_list[i];
        }
    }

    /* *#@*? */
    return NULL;
}

static void save_value(const char *file, int lineno, const char *name, const char *value)
{
    prte_mca_base_var_file_value_t *fv;
    bool found = false;

    /* First traverse through the list and ensure that we don't
     already have a param of this name.  If we do, just replace the
     value. */

    PRTE_LIST_FOREACH(fv, &prte_mca_base_var_file_values, prte_mca_base_var_file_value_t)
    {
        if (0 == strcmp(name, fv->mbvfv_var)) {
            if (NULL != fv->mbvfv_value) {
                free(fv->mbvfv_value);
            }
            found = true;
            break;
        }
    }

    if (!found) {
        /* We didn't already have the param, so append it to the list */
        fv = PRTE_NEW(prte_mca_base_var_file_value_t);
        if (NULL == fv) {
            return;
        }

        fv->mbvfv_var = strdup(name);
        prte_list_append(&prte_mca_base_var_file_values, &fv->super);
    }

    fv->mbvfv_value = value ? strdup(value) : NULL;
    fv->mbvfv_file = strdup(file);
    fv->mbvfv_lineno = lineno;
}

/*
 * Set it up
 */
int prte_mca_base_var_init(void)
{
    int ret;
    char *tmp;
    prte_mca_base_var_file_value_t *fv;

    if (!prte_mca_base_var_initialized) {
        /* Init the value array for the param storage */

        PRTE_CONSTRUCT(&prte_mca_base_vars, prte_pointer_array_t);
        /* These values are arbitrary */
        ret = prte_pointer_array_init(&prte_mca_base_vars, 128, 16384, 128);
        if (PRTE_SUCCESS != ret) {
            return ret;
        }

        prte_mca_base_var_count = 0;

        /* Init the file param value list */

        PRTE_CONSTRUCT(&prte_mca_base_var_file_values, prte_list_t);
        PRTE_CONSTRUCT(&prte_mca_base_envar_file_values, prte_list_t);
        PRTE_CONSTRUCT(&prte_mca_base_var_index_hash, prte_hash_table_t);

        ret = prte_hash_table_init(&prte_mca_base_var_index_hash, 1024);
        if (PRTE_SUCCESS != ret) {
            return ret;
        }

        ret = prte_mca_base_var_group_init();
        if (PRTE_SUCCESS != ret) {
            return ret;
        }

        /* Set this before we register the parameter, below */

        prte_mca_base_var_initialized = true;

        /* READ THE DEFAULT PARAMS FILE(S) AND PUSH THE RESULTS
         * INTO THE ENVIRONMENT */

        /* We may need this later */
        home = (char *) prte_home_directory();

        /* start with the system default param file */
        tmp = prte_os_path(false, prte_install_dirs.sysconfdir, "prte-mca-params.conf", NULL);
        ret = prte_util_keyval_parse(tmp, save_value);
        free(tmp);
        if (PRTE_SUCCESS != ret && PRTE_ERR_NOT_FOUND != ret) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }

#if PRTE_WANT_HOME_CONFIG_FILES
        /* do the user's home default param files */
        tmp = prte_os_path(false, home, ".prte", "prte-mca-params.conf", NULL);
        ret = prte_util_keyval_parse(tmp, save_value);
        free(tmp);
        if (PRTE_SUCCESS != ret && PRTE_ERR_NOT_FOUND != ret) {
            PRTE_ERROR_LOG(ret);
            return ret;
        }
#endif

        /* push the results into our environment, but do not overwrite
         * a value if the user already has it set as their environment
         * overrides anything from the default param files */
        PRTE_LIST_FOREACH(fv, &prte_mca_base_var_file_values, prte_mca_base_var_file_value_t)
        {
            prte_asprintf(&tmp, "PRTE_MCA_%s", fv->mbvfv_var);
            prte_setenv(tmp, fv->mbvfv_value, false, &environ);
            free(tmp);
        }
    }

    return PRTE_SUCCESS;
}

/*
 * Look up an integer MCA parameter.
 */
int prte_mca_base_var_get_value(int vari, const void *value, prte_mca_base_var_source_t *source,
                                const char **source_file)
{
    prte_mca_base_var_t *var;
    void **tmp = (void **) value;
    int ret;

    ret = var_get(vari, &var, true);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    if (!PRTE_VAR_IS_VALID(var[0])) {
        return PRTE_ERR_NOT_FOUND;
    }

    if (NULL != value) {
        /* Return a poiner to our backing store (either a char **, int *,
           or bool *) */
        *tmp = var->mbv_storage;
    }

    if (NULL != source) {
        *source = var->mbv_source;
    }

    if (NULL != source_file) {
        *source_file = prte_mca_base_var_source_file(var);
    }

    return PRTE_SUCCESS;
}

static int var_set_string(prte_mca_base_var_t *var, char *value)
{
    char *tmp;
    int ret;

    if (NULL != var->mbv_storage->stringval) {
        free(var->mbv_storage->stringval);
    }

    var->mbv_storage->stringval = NULL;

    if (NULL == value || 0 == strlen(value)) {
        return PRTE_SUCCESS;
    }

    /* Replace all instances of ~/ in a path-style string with the
       user's home directory. This may be handled by the enumerator
       in the future. */
    if (0 == strncmp(value, "~/", 2)) {
        if (NULL != home) {
            ret = prte_asprintf(&value, "%s/%s", home, value + 2);
            if (0 > ret) {
                return PRTE_ERROR;
            }
        } else {
            value = strdup(value + 2);
        }
    } else {
        value = strdup(value);
    }

    if (NULL == value) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    while (NULL != (tmp = strstr(value, ":~/"))) {
        tmp[0] = '\0';
        tmp += 3;

        ret = prte_asprintf(&tmp, "%s:%s%s%s", value, home ? home : "", home ? "/" : "", tmp);

        free(value);

        if (0 > ret) {
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        value = tmp;
    }

    var->mbv_storage->stringval = value;

    return PRTE_SUCCESS;
}

static int int_from_string(const char *src, prte_mca_base_var_enum_t *enumerator,
                           uint64_t *value_out)
{
    uint64_t value;
    bool is_int;
    char *tmp;

    if (NULL == src || 0 == strlen(src)) {
        if (NULL == enumerator) {
            *value_out = 0;
        }

        return PRTE_SUCCESS;
    }

    if (enumerator) {
        int int_val, ret;
        ret = enumerator->value_from_string(enumerator, src, &int_val);
        if (PRTE_SUCCESS != ret) {
            return ret;
        }
        *value_out = (uint64_t) int_val;

        return PRTE_SUCCESS;
    }

    /* Check for an integer value */
    value = strtoull(src, &tmp, 0);
    is_int = tmp[0] == '\0';

    if (!is_int && tmp != src) {
        switch (tmp[0]) {
        case 'G':
        case 'g':
            value <<= 10;
        case 'M':
        case 'm':
            value <<= 10;
        case 'K':
        case 'k':
            value <<= 10;
            break;
        default:
            break;
        }
    }

    *value_out = value;

    return PRTE_SUCCESS;
}

static int var_set_from_string(prte_mca_base_var_t *var, char *src)
{
    prte_mca_base_var_storage_t *dst = var->mbv_storage;
    uint64_t int_value = 0;
    int ret;

    switch (var->mbv_type) {
    case PRTE_MCA_BASE_VAR_TYPE_INT:
    case PRTE_MCA_BASE_VAR_TYPE_INT32_T:
    case PRTE_MCA_BASE_VAR_TYPE_UINT32_T:
    case PRTE_MCA_BASE_VAR_TYPE_LONG:
    case PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT:
    case PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG:
    case PRTE_MCA_BASE_VAR_TYPE_INT64_T:
    case PRTE_MCA_BASE_VAR_TYPE_UINT64_T:
    case PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG_LONG:
    case PRTE_MCA_BASE_VAR_TYPE_BOOL:
    case PRTE_MCA_BASE_VAR_TYPE_SIZE_T:
        ret = int_from_string(src, var->mbv_enumerator, &int_value);
        if (PRTE_SUCCESS != ret
            || (PRTE_MCA_BASE_VAR_TYPE_INT == var->mbv_type
                && ((int) int_value != (int64_t) int_value))
            || (PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT == var->mbv_type
                && ((unsigned int) int_value != int_value))) {
            if (var->mbv_enumerator) {
                char *valid_values;
                (void) var->mbv_enumerator->dump(var->mbv_enumerator, &valid_values);
                prte_show_help("help-prte-mca-var.txt", "invalid-value-enum", true,
                               var->mbv_full_name, src, valid_values);
                free(valid_values);
            } else {
                prte_show_help("help-prte-mca-var.txt", "invalid-value", true, var->mbv_full_name,
                               src);
            }

            return PRTE_ERR_VALUE_OUT_OF_BOUNDS;
        }

        if (PRTE_MCA_BASE_VAR_TYPE_INT == var->mbv_type
            || PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT == var->mbv_type) {
            int *castme = (int *) var->mbv_storage;
            *castme = int_value;
        } else if (PRTE_MCA_BASE_VAR_TYPE_INT32_T == var->mbv_type
                   || PRTE_MCA_BASE_VAR_TYPE_UINT32_T == var->mbv_type) {
            int32_t *castme = (int32_t *) var->mbv_storage;
            *castme = int_value;
        } else if (PRTE_MCA_BASE_VAR_TYPE_INT64_T == var->mbv_type
                   || PRTE_MCA_BASE_VAR_TYPE_UINT64_T == var->mbv_type) {
            int64_t *castme = (int64_t *) var->mbv_storage;
            *castme = int_value;
        } else if (PRTE_MCA_BASE_VAR_TYPE_LONG == var->mbv_type) {
            long *castme = (long *) var->mbv_storage;
            *castme = (long) int_value;
        } else if (PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG == var->mbv_type) {
            unsigned long *castme = (unsigned long *) var->mbv_storage;
            *castme = (unsigned long) int_value;
        } else if (PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG_LONG == var->mbv_type) {
            unsigned long long *castme = (unsigned long long *) var->mbv_storage;
            *castme = (unsigned long long) int_value;
        } else if (PRTE_MCA_BASE_VAR_TYPE_SIZE_T == var->mbv_type) {
            size_t *castme = (size_t *) var->mbv_storage;
            *castme = (size_t) int_value;
        } else if (PRTE_MCA_BASE_VAR_TYPE_BOOL == var->mbv_type) {
            bool *castme = (bool *) var->mbv_storage;
            *castme = !!int_value;
        }

        return ret;
    case PRTE_MCA_BASE_VAR_TYPE_DOUBLE:
        dst->lfval = strtod(src, NULL);
        break;
    case PRTE_MCA_BASE_VAR_TYPE_STRING:
    case PRTE_MCA_BASE_VAR_TYPE_VERSION_STRING:
        var_set_string(var, src);
        break;
    case PRTE_MCA_BASE_VAR_TYPE_MAX:
        return PRTE_ERROR;
    }

    return PRTE_SUCCESS;
}

/*
 * Set a variable
 */
int prte_mca_base_var_set_value(int vari, const void *value, size_t size,
                                prte_mca_base_var_source_t source, const char *source_file)
{
    prte_mca_base_var_t *var;
    int ret;

    ret = var_get(vari, &var, true);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    if (!PRTE_VAR_IS_VALID(var[0])) {
        return PRTE_ERR_BAD_PARAM;
    }

    if (!PRTE_VAR_IS_SETTABLE(var[0])) {
        return PRTE_ERR_PERM;
    }

    if (NULL != var->mbv_enumerator) {
        /* Validate */
        ret = var->mbv_enumerator->string_from_value(var->mbv_enumerator, ((int *) value)[0], NULL);
        if (PRTE_SUCCESS != ret) {
            return ret;
        }
    }

    if (PRTE_MCA_BASE_VAR_TYPE_STRING != var->mbv_type
        && PRTE_MCA_BASE_VAR_TYPE_VERSION_STRING != var->mbv_type) {
        memmove(var->mbv_storage, value, prte_var_type_sizes[var->mbv_type]);
    } else {
        var_set_string(var, (char *) value);
    }

    var->mbv_source = source;

    if (PRTE_MCA_BASE_VAR_SOURCE_FILE == source && NULL != source_file) {
        var->mbv_file_value = NULL;
        var->mbv_source_file = append_filename_to_list(source_file);
    }

    return PRTE_SUCCESS;
}

/*
 * Deregister a parameter
 */
int prte_mca_base_var_deregister(int vari)
{
    prte_mca_base_var_t *var;
    int ret;

    ret = var_get(vari, &var, false);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    if (!PRTE_VAR_IS_VALID(var[0])) {
        return PRTE_ERR_BAD_PARAM;
    }

    /* Mark this parameter as invalid but keep its info in case this
       parameter is reregistered later */
    var->mbv_flags &= ~PRTE_MCA_BASE_VAR_FLAG_VALID;

    /* Done deregistering synonym */
    if (PRTE_MCA_BASE_VAR_FLAG_SYNONYM & var->mbv_flags) {
        return PRTE_SUCCESS;
    }

    /* Release the current value if it is a string. */
    if ((PRTE_MCA_BASE_VAR_TYPE_STRING == var->mbv_type
         || PRTE_MCA_BASE_VAR_TYPE_VERSION_STRING == var->mbv_type)
        && var->mbv_storage->stringval) {
        free(var->mbv_storage->stringval);
        var->mbv_storage->stringval = NULL;
    } else {
        PRTE_MCA_VAR_MBV_ENUMERATOR_FREE(var->mbv_enumerator);
    }

    var->mbv_enumerator = NULL;

    var->mbv_storage = NULL;

    return PRTE_SUCCESS;
}

static int var_get(int vari, prte_mca_base_var_t **var_out, bool original)
{
    prte_mca_base_var_t *var;

    if (var_out) {
        *var_out = NULL;
    }

    /* Check for bozo cases */
    if (!prte_mca_base_var_initialized) {
        return PRTE_ERROR;
    }

    if (vari < 0) {
        return PRTE_ERR_BAD_PARAM;
    }

    var = prte_pointer_array_get_item(&prte_mca_base_vars, vari);
    if (NULL == var) {
        return PRTE_ERR_BAD_PARAM;
    }

    if (PRTE_VAR_IS_SYNONYM(var[0]) && original) {
        return var_get(var->mbv_synonym_for, var_out, false);
    }

    if (var_out) {
        *var_out = var;
    }

    return PRTE_SUCCESS;
}

int prte_mca_base_var_env_name(const char *param_name, char **env_name)
{
    int ret;

    assert(NULL != env_name);

    ret = prte_asprintf(env_name, "%s%s", prte_mca_prefix, param_name);
    if (0 > ret) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    return PRTE_SUCCESS;
}

/*
 * Find the index for an MCA parameter based on its names.
 */
static int var_find_by_name(const char *full_name, int *vari, bool invalidok)
{
    prte_mca_base_var_t *var = NULL;
    void *tmp;
    int rc;

    rc = prte_hash_table_get_value_ptr(&prte_mca_base_var_index_hash, full_name, strlen(full_name),
                                       &tmp);
    if (PRTE_SUCCESS != rc) {
        return rc;
    }

    (void) var_get((int) (uintptr_t) tmp, &var, false);

    if (invalidok || (var && PRTE_VAR_IS_VALID(var[0]))) {
        *vari = (int) (uintptr_t) tmp;
        return PRTE_SUCCESS;
    }

    return PRTE_ERR_NOT_FOUND;
}

static int var_find(const char *project_name, const char *framework_name,
                    const char *component_name, const char *variable_name, bool invalidok)
{
    char *full_name;
    int ret, vari;

    ret = prte_mca_base_var_generate_full_name4(NULL, framework_name, component_name, variable_name,
                                                &full_name);
    if (PRTE_SUCCESS != ret) {
        return PRTE_ERROR;
    }

    ret = var_find_by_name(full_name, &vari, invalidok);

    /* NTH: should we verify the name components match? */

    free(full_name);

    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    return vari;
}

/*
 * Find the index for an MCA parameter based on its name components.
 */
int prte_mca_base_var_find(const char *project_name, const char *framework_name,
                           const char *component_name, const char *variable_name)
{
    return var_find(project_name, framework_name, component_name, variable_name, false);
}

/*
 * Find the index for an MCA parameter based on full name.
 */
int prte_mca_base_var_find_by_name(const char *full_name, int *vari)
{
    return var_find_by_name(full_name, vari, false);
}

int prte_mca_base_var_set_flag(int vari, prte_mca_base_var_flag_t flag, bool set)
{
    prte_mca_base_var_t *var;
    int ret;

    ret = var_get(vari, &var, true);
    if (PRTE_SUCCESS != ret || PRTE_VAR_IS_SYNONYM(var[0])) {
        return PRTE_ERR_BAD_PARAM;
    }

    var->mbv_flags = (var->mbv_flags & ~flag) | (set ? flag : PRTE_MCA_BASE_VAR_FLAG_NONE);

    /* All done */
    return PRTE_SUCCESS;
}

/*
 * Return info on a parameter at an index
 */
int prte_mca_base_var_get(int vari, const prte_mca_base_var_t **var)
{
    int ret;
    ret = var_get(vari, (prte_mca_base_var_t **) var, false);

    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    if (!PRTE_VAR_IS_VALID(*(var[0]))) {
        return PRTE_ERR_NOT_FOUND;
    }

    return PRTE_SUCCESS;
}

/*
 * Make an argv-style list of strings suitable for an environment
 */
int prte_mca_base_var_build_env(char ***env, int *num_env, bool internal)
{
    prte_mca_base_var_t *var;
    size_t i, len;
    int ret;

    /* Check for bozo cases */

    if (!prte_mca_base_var_initialized) {
        return PRTE_ERROR;
    }

    /* Iterate through all the registered parameters */

    len = prte_pointer_array_get_size(&prte_mca_base_vars);
    for (i = 0; i < len; ++i) {
        char *value_string;
        char *str = NULL;

        var = prte_pointer_array_get_item(&prte_mca_base_vars, i);
        if (NULL == var) {
            continue;
        }

        /* Don't output default values or internal variables (unless
           requested) */
        if (PRTE_MCA_BASE_VAR_SOURCE_DEFAULT == var->mbv_source
            || (!internal && PRTE_VAR_IS_INTERNAL(var[0]))) {
            continue;
        }

        if ((PRTE_MCA_BASE_VAR_TYPE_STRING == var->mbv_type
             || PRTE_MCA_BASE_VAR_TYPE_VERSION_STRING == var->mbv_type)
            && NULL == var->mbv_storage->stringval) {
            continue;
        }

        ret = var_value_string(var, &value_string);
        if (PRTE_SUCCESS != ret) {
            goto cleanup;
        }

        ret = prte_asprintf(&str, "%s%s=%s", prte_mca_prefix, var->mbv_full_name, value_string);
        free(value_string);
        if (0 > ret) {
            goto cleanup;
        }

        prte_argv_append(num_env, env, str);
        free(str);

        switch (var->mbv_source) {
        case PRTE_MCA_BASE_VAR_SOURCE_FILE:
        case PRTE_MCA_BASE_VAR_SOURCE_OVERRIDE:
            prte_asprintf(&str, "%sSOURCE_%s=FILE:%s", prte_mca_prefix, var->mbv_full_name,
                          prte_mca_base_var_source_file(var));
            break;
        case PRTE_MCA_BASE_VAR_SOURCE_COMMAND_LINE:
            prte_asprintf(&str, "%sSOURCE_%s=COMMAND_LINE", prte_mca_prefix, var->mbv_full_name);
            break;
        case PRTE_MCA_BASE_VAR_SOURCE_ENV:
        case PRTE_MCA_BASE_VAR_SOURCE_SET:
        case PRTE_MCA_BASE_VAR_SOURCE_DEFAULT:
            str = NULL;
            break;
        case PRTE_MCA_BASE_VAR_SOURCE_MAX:
            goto cleanup;
        }

        if (NULL != str) {
            prte_argv_append(num_env, env, str);
            free(str);
        }
    }

    /* All done */

    return PRTE_SUCCESS;

    /* Error condition */

cleanup:
    if (*num_env > 0) {
        prte_argv_free(*env);
        *num_env = 0;
        *env = NULL;
    }
    return PRTE_ERR_NOT_FOUND;
}

/*
 * Shut down the MCA parameter system (normally only invoked by the
 * MCA framework itself).
 */
void prte_mca_base_var_finalize(void)
{
    prte_object_t *object;
    prte_list_item_t *item;
    int size, i;

    if (prte_mca_base_var_initialized) {
        size = prte_pointer_array_get_size(&prte_mca_base_vars);
        for (i = 0; i < size; ++i) {
            object = prte_pointer_array_get_item(&prte_mca_base_vars, i);
            if (NULL != object) {
                PRTE_RELEASE(object);
            }
        }
        PRTE_DESTRUCT(&prte_mca_base_vars);

        while (NULL != (item = prte_list_remove_first(&prte_mca_base_var_file_values))) {
            PRTE_RELEASE(item);
        }
        PRTE_DESTRUCT(&prte_mca_base_var_file_values);

        while (NULL != (item = prte_list_remove_first(&prte_mca_base_envar_file_values))) {
            PRTE_RELEASE(item);
        }
        PRTE_DESTRUCT(&prte_mca_base_envar_file_values);

        prte_mca_base_var_initialized = false;
        prte_mca_base_var_count = 0;

        if (NULL != prte_mca_base_var_file_list) {
            prte_argv_free(prte_mca_base_var_file_list);
        }
        prte_mca_base_var_file_list = NULL;

        (void) prte_mca_base_var_group_finalize();

        PRTE_DESTRUCT(&prte_mca_base_var_index_hash);

        free(prte_mca_base_envar_files);
        prte_mca_base_envar_files = NULL;
    }
}

/******************************************************************************/
static int register_variable(const char *project_name, const char *framework_name,
                             const char *component_name, const char *variable_name,
                             const char *description, prte_mca_base_var_type_t type,
                             prte_mca_base_var_enum_t *enumerator, int bind,
                             prte_mca_base_var_flag_t flags, prte_mca_base_var_info_lvl_t info_lvl,
                             prte_mca_base_var_scope_t scope, int synonym_for, void *storage)
{
    int ret, var_index, group_index, tmp;
    prte_mca_base_var_group_t *group;
    prte_mca_base_var_t *var, *original = NULL;

    /* Developer error. Storage can not be NULL and type must exist */
    assert(((flags & PRTE_MCA_BASE_VAR_FLAG_SYNONYM) || NULL != storage) && type >= 0
           && type < PRTE_MCA_BASE_VAR_TYPE_MAX);

    /* Developer error: check max length of strings */
    if (NULL != project_name && strlen(project_name) > PRTE_MCA_BASE_MAX_PROJECT_NAME_LEN) {
        return PRTE_ERR_BAD_PARAM;
    }
    if (NULL != framework_name && strlen(framework_name) > PRTE_MCA_BASE_MAX_TYPE_NAME_LEN) {
        return PRTE_ERR_BAD_PARAM;
    }
    if (NULL != component_name && strlen(component_name) > PRTE_MCA_BASE_MAX_COMPONENT_NAME_LEN) {
        return PRTE_ERR_BAD_PARAM;
    }
    if (NULL != variable_name && strlen(variable_name) > PRTE_MCA_BASE_MAX_VARIABLE_NAME_LEN) {
        return PRTE_ERR_BAD_PARAM;
    }

#if PRTE_ENABLE_DEBUG
    /* Developer error: check for alignments */
    uintptr_t align = 0;
    switch (type) {
    case PRTE_MCA_BASE_VAR_TYPE_INT:
    case PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT:
        align = PRTE_ALIGNMENT_INT;
        break;
    case PRTE_MCA_BASE_VAR_TYPE_INT32_T:
    case PRTE_MCA_BASE_VAR_TYPE_UINT32_T:
        align = PRTE_ALIGNMENT_INT32;
        break;
    case PRTE_MCA_BASE_VAR_TYPE_INT64_T:
    case PRTE_MCA_BASE_VAR_TYPE_UINT64_T:
        align = PRTE_ALIGNMENT_INT64;
        break;
    case PRTE_MCA_BASE_VAR_TYPE_LONG:
    case PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG:
        align = PRTE_ALIGNMENT_LONG;
        break;
    case PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG_LONG:
        align = PRTE_ALIGNMENT_LONG_LONG;
        break;
    case PRTE_MCA_BASE_VAR_TYPE_SIZE_T:
        align = PRTE_ALIGNMENT_SIZE_T;
        break;
    case PRTE_MCA_BASE_VAR_TYPE_BOOL:
        align = PRTE_ALIGNMENT_BOOL;
        break;
    case PRTE_MCA_BASE_VAR_TYPE_DOUBLE:
        align = PRTE_ALIGNMENT_DOUBLE;
        break;
    case PRTE_MCA_BASE_VAR_TYPE_VERSION_STRING:
    case PRTE_MCA_BASE_VAR_TYPE_STRING:
    default:
        align = 0;
        break;
    }

    if (0 != align) {
        assert(((uintptr_t) storage) % align == 0);
    }

    /* Also check to ensure that synonym_for>=0 when
       MCA_BCASE_VAR_FLAG_SYNONYM is specified */
    if (flags & PRTE_MCA_BASE_VAR_FLAG_SYNONYM && synonym_for < 0) {
        assert((flags & PRTE_MCA_BASE_VAR_FLAG_SYNONYM) && synonym_for >= 0);
    }
#endif

    if (flags & PRTE_MCA_BASE_VAR_FLAG_SYNONYM) {
        original = prte_pointer_array_get_item(&prte_mca_base_vars, synonym_for);
        if (NULL == original) {
            /* Attempting to create a synonym for a non-existent variable. probably a
             * developer error. */
            assert(NULL != original);
            return PRTE_ERR_NOT_FOUND;
        }
    }

    /* There are data holes in the var struct */
    PRTE_DEBUG_ZERO(var);

    /* Initialize the array if it has never been initialized */
    if (!prte_mca_base_var_initialized) {
        prte_mca_base_var_init();
    }

    /* See if this entry is already in the array */
    var_index = var_find(project_name, framework_name, component_name, variable_name, true);

    if (0 > var_index) {
        /* Create a new parameter entry */
        group_index = prte_mca_base_var_group_register(project_name, framework_name, component_name,
                                                       NULL);
        if (-1 > group_index) {
            return group_index;
        }

        /* Read-only and constant variables can't be settable */
        if (scope < PRTE_MCA_BASE_VAR_SCOPE_LOCAL
            || (flags & PRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY)) {
            if ((flags & PRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY)
                && (flags & PRTE_MCA_BASE_VAR_FLAG_SETTABLE)) {
                prte_show_help("help-prte-mca-var.txt", "invalid-flag-combination", true,
                               "PRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY",
                               "PRTE_MCA_BASE_VAR_FLAG_SETTABLE");
                return PRTE_ERROR;
            }

            /* Should we print a warning for other cases? */
            flags &= ~PRTE_MCA_BASE_VAR_FLAG_SETTABLE;
        }

        var = PRTE_NEW(prte_mca_base_var_t);

        var->mbv_type = type;
        var->mbv_flags = flags;
        var->mbv_group_index = group_index;
        var->mbv_info_lvl = info_lvl;
        var->mbv_scope = scope;
        var->mbv_synonym_for = synonym_for;
        var->mbv_bind = bind;

        if (NULL != description) {
            var->mbv_description = strdup(description);
        }

        ret = prte_mca_base_var_generate_full_name4(project_name, framework_name, component_name,
                                                    variable_name, &var->mbv_long_name);
        if (PRTE_SUCCESS != ret) {
            PRTE_RELEASE(var);
            return PRTE_ERROR;
        }
        /* The mbv_full_name and the variable name are subset of the mbv_long_name
         * so instead of allocating them we can just point into the var mbv_long_name
         * at the right location.
         */
        var->mbv_full_name = var->mbv_long_name
                             + (NULL == project_name ? 0
                                                     : (strlen(project_name) + 1)); /* 1 for _ */
        if (NULL != variable_name) {
            var->mbv_variable_name = var->mbv_full_name
                                     + (NULL == framework_name ? 0 : (strlen(framework_name) + 1))
                                     + (NULL == component_name ? 0 : (strlen(component_name) + 1));
        }

        /* Add it to the array.  Note that we copy the mca_var_t by value,
           so the entire contents of the struct is copied.  The synonym list
           will always be empty at this point, so there's no need for an
           extra RETAIN or RELEASE. */
        var_index = prte_pointer_array_add(&prte_mca_base_vars, var);
        if (0 > var_index) {
            PRTE_RELEASE(var);
            return PRTE_ERROR;
        }

        var->mbv_index = var_index;

        if (0 <= group_index) {
            prte_mca_base_var_group_add_var(group_index, var_index);
        }

        prte_mca_base_var_count++;
        if (0 <= var_find_by_name(var->mbv_full_name, &tmp, 0)) {
            /* XXX --- FIXME: variable overshadows an existing variable. this is difficult to
             * support */
            assert(0);
        }

        prte_hash_table_set_value_ptr(&prte_mca_base_var_index_hash, var->mbv_full_name,
                                      strlen(var->mbv_full_name), (void *) (uintptr_t) var_index);
    } else {
        ret = var_get(var_index, &var, false);
        if (PRTE_SUCCESS != ret) {
            /* Shouldn't ever happen */
            return PRTE_ERROR;
        }

        ret = prte_mca_base_var_group_get_internal(var->mbv_group_index, &group, true);
        if (PRTE_SUCCESS != ret) {
            /* Shouldn't ever happen */
            return PRTE_ERROR;
        }

        if (!group->group_isvalid) {
            group->group_isvalid = true;
        }

        /* Verify the name components match */
        if (0 != compare_strings(framework_name, group->group_framework)
            || 0 != compare_strings(component_name, group->group_component)
            || 0 != compare_strings(variable_name, var->mbv_variable_name)) {
            prte_show_help("help-prte-mca-var.txt", "var-name-conflict", true, var->mbv_full_name,
                           framework_name, component_name, variable_name, group->group_framework,
                           group->group_component, var->mbv_variable_name);
            /* This is developer error. abort! */
            assert(0);
            return PRTE_ERROR;
        }

        if (var->mbv_type != type) {
#if PRTE_ENABLE_DEBUG
            prte_show_help("help-prte-mca-var.txt", "re-register-with-different-type", true,
                           var->mbv_full_name);
#endif
            return PRTE_ERR_VALUE_OUT_OF_BOUNDS;
        }
    }

    if (PRTE_MCA_BASE_VAR_TYPE_BOOL == var->mbv_type) {
        enumerator = &prte_mca_base_var_enum_bool;
    } else if (NULL != enumerator) {
        PRTE_MCA_VAR_MBV_ENUMERATOR_FREE(var->mbv_enumerator);
        if (!enumerator->enum_is_static) {
            PRTE_RETAIN(enumerator);
        }
    }

    var->mbv_enumerator = enumerator;

    if (!original) {
        var->mbv_storage = storage;

        /* make a copy of the default string value */
        if ((PRTE_MCA_BASE_VAR_TYPE_STRING == type || PRTE_MCA_BASE_VAR_TYPE_VERSION_STRING == type)
            && NULL != ((char **) storage)[0]) {
            ((char **) storage)[0] = strdup(((char **) storage)[0]);
        }
    } else {
        /* synonym variable */
        prte_value_array_append_item(&original->mbv_synonyms, &var_index);
    }

    /* go ahead and mark this variable as valid */
    var->mbv_flags |= PRTE_MCA_BASE_VAR_FLAG_VALID;

    ret = var_set_initial(var, original);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    /* All done */
    return var_index;
}

int prte_mca_base_var_register(const char *project_name, const char *framework_name,
                               const char *component_name, const char *variable_name,
                               const char *description, prte_mca_base_var_type_t type,
                               prte_mca_base_var_enum_t *enumerator, int bind,
                               prte_mca_base_var_flag_t flags,
                               prte_mca_base_var_info_lvl_t info_lvl,
                               prte_mca_base_var_scope_t scope, void *storage)
{
    int ret;

    /* Only integer variables can have enumerator */
    assert(NULL == enumerator
           || (PRTE_MCA_BASE_VAR_TYPE_INT == type || PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT == type));

    ret = register_variable(project_name, framework_name, component_name, variable_name,
                            description, type, enumerator, bind, flags, info_lvl, scope, -1,
                            storage);

    if (PRTE_UNLIKELY(0 > ret)) {
        return ret;
    }

    /* Register aliases if any exist */
    const prte_mca_base_alias_t *alias = prte_mca_base_alias_lookup(project_name, framework_name,
                                                                    component_name);
    if (NULL == alias) {
        return ret;
    }

    PRTE_LIST_FOREACH_DECL(alias_item, &alias->component_aliases, prte_mca_base_alias_item_t)
    {
        prte_mca_base_var_syn_flag_t inflags = PRTE_MCA_BASE_VAR_SYN_FLAG_NONE;
        if (alias_item->alias_flags & PRTE_MCA_BASE_ALIAS_FLAG_DEPRECATED) {
            inflags = PRTE_MCA_BASE_VAR_SYN_FLAG_DEPRECATED;
        }
        (void) prte_mca_base_var_register_synonym(ret, project_name, framework_name,
                                                  alias_item->component_alias, variable_name,
                                                  inflags);
    }

    return ret;
}

int prte_mca_base_component_var_register(const prte_mca_base_component_t *component,
                                         const char *variable_name, const char *description,
                                         prte_mca_base_var_type_t type,
                                         prte_mca_base_var_enum_t *enumerator, int bind,
                                         prte_mca_base_var_flag_t flags,
                                         prte_mca_base_var_info_lvl_t info_lvl,
                                         prte_mca_base_var_scope_t scope, void *storage)
{
    return prte_mca_base_var_register(component->mca_project_name, component->mca_type_name,
                                      component->mca_component_name, variable_name, description,
                                      type, enumerator, bind, flags | PRTE_MCA_BASE_VAR_FLAG_DWG,
                                      info_lvl, scope, storage);
}

int prte_mca_base_framework_var_register(const prte_mca_base_framework_t *framework,
                                         const char *variable_name, const char *help_msg,
                                         prte_mca_base_var_type_t type,
                                         prte_mca_base_var_enum_t *enumerator, int bind,
                                         prte_mca_base_var_flag_t flags,
                                         prte_mca_base_var_info_lvl_t info_level,
                                         prte_mca_base_var_scope_t scope, void *storage)
{
    return prte_mca_base_var_register(framework->framework_project, framework->framework_name,
                                      "base", variable_name, help_msg, type, enumerator, bind,
                                      flags | PRTE_MCA_BASE_VAR_FLAG_DWG, info_level, scope,
                                      storage);
}

int prte_mca_base_var_register_synonym(int synonym_for, const char *project_name,
                                       const char *framework_name, const char *component_name,
                                       const char *synonym_name, prte_mca_base_var_syn_flag_t flags)
{
    prte_mca_base_var_flag_t var_flags = (prte_mca_base_var_flag_t) PRTE_MCA_BASE_VAR_FLAG_SYNONYM;
    prte_mca_base_var_t *var;
    int ret;

    ret = var_get(synonym_for, &var, false);
    if (PRTE_SUCCESS != ret || PRTE_VAR_IS_SYNONYM(var[0])) {
        return PRTE_ERR_BAD_PARAM;
    }

    if (flags & PRTE_MCA_BASE_VAR_SYN_FLAG_DEPRECATED) {
        var_flags |= PRTE_MCA_BASE_VAR_FLAG_DEPRECATED;
    }
    if (flags & PRTE_MCA_BASE_VAR_SYN_FLAG_INTERNAL) {
        var_flags |= PRTE_MCA_BASE_VAR_FLAG_INTERNAL;
    }

    return register_variable(project_name, framework_name, component_name, synonym_name,
                             var->mbv_description, var->mbv_type, var->mbv_enumerator,
                             var->mbv_bind, var_flags, var->mbv_info_lvl, var->mbv_scope,
                             synonym_for, NULL);
}

static int var_get_env(prte_mca_base_var_t *var, const char *name, char **source, char **value)
{
    const char source_prefix[] = "SOURCE_";
    const int max_len = strlen(prte_mca_prefix) + strlen(source_prefix) + strlen(name) + 1;
    char *envvar = alloca(max_len);
    if (NULL == envvar) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    int ret;
    ret = snprintf(envvar, max_len, "%s%s", prte_mca_prefix, name);
    if (0 > ret) {
        return PRTE_ERROR;
    }
    *value = getenv(envvar);
    if (NULL == *value) {
        *source = NULL;
        return PRTE_ERR_NOT_FOUND;
    }

    ret = snprintf(envvar, max_len, "%s%s%s", prte_mca_prefix, source_prefix, name);
    if (0 > ret) {
        return PRTE_ERROR;
    }
    *source = getenv(envvar);

    return PRTE_SUCCESS;
}

/*
 * Lookup a param in the environment
 */
static int var_set_from_env(prte_mca_base_var_t *var, prte_mca_base_var_t *original)
{
    const char *var_full_name = var->mbv_full_name;
    const char *var_long_name = var->mbv_long_name;
    bool deprecated = PRTE_VAR_IS_DEPRECATED(var[0]);
    bool is_synonym = PRTE_VAR_IS_SYNONYM(var[0]);
    char *source_env, *value_env;
    int ret;

    ret = var_get_env(var, var_long_name, &source_env, &value_env);
    if (PRTE_SUCCESS != ret) {
        ret = var_get_env(var, var_full_name, &source_env, &value_env);
    }

    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    /* we found an environment variable but this variable is default-only. print
       a warning. */
    if (PRTE_VAR_IS_DEFAULT_ONLY(original[0])) {
        prte_show_help("help-prte-mca-var.txt", "default-only-param-set", true, var_full_name);

        return PRTE_ERR_NOT_FOUND;
    }

    if (PRTE_MCA_BASE_VAR_SOURCE_OVERRIDE == original->mbv_source) {
        if (!prte_mca_base_var_suppress_override_warning) {
            prte_show_help("help-prte-mca-var.txt", "overridden-param-set", true, var_full_name);
        }

        return PRTE_ERR_NOT_FOUND;
    }

    original->mbv_source = PRTE_MCA_BASE_VAR_SOURCE_ENV;

    if (NULL != source_env) {
        if (0 == strncasecmp(source_env, "file:", 5)) {
            original->mbv_source_file = append_filename_to_list(source_env + 5);
            original->mbv_source = PRTE_MCA_BASE_VAR_SOURCE_FILE;
        } else if (0 == strcasecmp(source_env, "command")) {
            var->mbv_source = PRTE_MCA_BASE_VAR_SOURCE_COMMAND_LINE;
        }
    }

    if (deprecated) {
        const char *new_variable = "None (going away)";

        if (is_synonym) {
            new_variable = original->mbv_full_name;
        }

        switch (var->mbv_source) {
        case PRTE_MCA_BASE_VAR_SOURCE_ENV:
            prte_show_help("help-prte-mca-var.txt", "deprecated-mca-env", true, var_full_name,
                           new_variable);
            break;
        case PRTE_MCA_BASE_VAR_SOURCE_COMMAND_LINE:
            prte_show_help("help-prte-mca-var.txt", "deprecated-mca-cli", true, var_full_name,
                           new_variable);
            break;
        case PRTE_MCA_BASE_VAR_SOURCE_FILE:
        case PRTE_MCA_BASE_VAR_SOURCE_OVERRIDE:
            prte_show_help("help-prte-mca-var.txt", "deprecated-mca-file", true, var_full_name,
                           prte_mca_base_var_source_file(var), new_variable);
            break;

        case PRTE_MCA_BASE_VAR_SOURCE_DEFAULT:
        case PRTE_MCA_BASE_VAR_SOURCE_MAX:
        case PRTE_MCA_BASE_VAR_SOURCE_SET:
            /* silence compiler warnings about unhandled enumerations */
            break;
        }
    }

    return var_set_from_string(original, value_env);
}

/*
 * Lookup a param in the files
 */
static int var_set_from_file(prte_mca_base_var_t *var, prte_mca_base_var_t *original,
                             prte_list_t *file_values)
{
    const char *var_full_name = var->mbv_full_name;
    const char *var_long_name = var->mbv_long_name;
    bool deprecated = PRTE_VAR_IS_DEPRECATED(var[0]);
    bool is_synonym = PRTE_VAR_IS_SYNONYM(var[0]);
    prte_mca_base_var_file_value_t *fv;

    /* Scan through the list of values read in from files and try to
       find a match.  If we do, cache it on the param (for future
       lookups) and save it in the storage. */

    PRTE_LIST_FOREACH(fv, file_values, prte_mca_base_var_file_value_t)
    {
        if (0 != strcmp(fv->mbvfv_var, var_full_name)
            && 0 != strcmp(fv->mbvfv_var, var_long_name)) {
            continue;
        }

        /* found it */
        if (PRTE_VAR_IS_DEFAULT_ONLY(var[0])) {
            prte_show_help("help-prte-mca-var.txt", "default-only-param-set", true, var_full_name);

            return PRTE_ERR_NOT_FOUND;
        }

        if (PRTE_MCA_BASE_VAR_FLAG_ENVIRONMENT_ONLY & original->mbv_flags) {
            prte_show_help("help-prte-mca-var.txt", "environment-only-param", true, var_full_name,
                           fv->mbvfv_value, fv->mbvfv_file);

            return PRTE_ERR_NOT_FOUND;
        }

        if (PRTE_MCA_BASE_VAR_SOURCE_OVERRIDE == original->mbv_source) {
            if (!prte_mca_base_var_suppress_override_warning) {
                prte_show_help("help-prte-mca-var.txt", "overridden-param-set", true,
                               var_full_name);
            }

            return PRTE_ERR_NOT_FOUND;
        }

        if (deprecated) {
            const char *new_variable = "None (going away)";

            if (is_synonym) {
                new_variable = original->mbv_full_name;
            }

            prte_show_help("help-prte-mca-var.txt", "deprecated-mca-file", true, var_full_name,
                           fv->mbvfv_file, new_variable);
        }

        original->mbv_file_value = (void *) fv;
        original->mbv_source = PRTE_MCA_BASE_VAR_SOURCE_FILE;
        if (is_synonym) {
            var->mbv_file_value = (void *) fv;
            var->mbv_source = PRTE_MCA_BASE_VAR_SOURCE_FILE;
        }

        return var_set_from_string(original, fv->mbvfv_value);
    }

    return PRTE_ERR_NOT_FOUND;
}

/*
 * Lookup the initial value for a parameter
 */
static int var_set_initial(prte_mca_base_var_t *var, prte_mca_base_var_t *original)
{
    int ret;

    if (original) {
        /* synonym */
        var->mbv_source = original->mbv_source;
        var->mbv_file_value = original->mbv_file_value;
        var->mbv_source_file = original->mbv_source_file;
    } else {
        var->mbv_source = PRTE_MCA_BASE_VAR_SOURCE_DEFAULT;
        original = var;
    }

    /* Check all the places that the param may be hiding, in priority
       order. If the default only flag is set the user will get a
       warning if they try to set a value from the environment or a
       file. */
    ret = var_set_from_env(var, original);
    if (PRTE_ERR_NOT_FOUND != ret) {
        return ret;
    }

    ret = var_set_from_file(var, original, &prte_mca_base_envar_file_values);
    if (PRTE_ERR_NOT_FOUND != ret) {
        return ret;
    }

    ret = var_set_from_file(var, original, &prte_mca_base_var_file_values);
    if (PRTE_ERR_NOT_FOUND != ret) {
        return ret;
    }

    return PRTE_SUCCESS;
}

/*
 * Create an empty param container
 */
static void var_constructor(prte_mca_base_var_t *var)
{
    memset((char *) var + sizeof(var->super), 0, sizeof(*var) - sizeof(var->super));

    var->mbv_type = PRTE_MCA_BASE_VAR_TYPE_MAX;
    PRTE_CONSTRUCT(&var->mbv_synonyms, prte_value_array_t);
    prte_value_array_init(&var->mbv_synonyms, sizeof(int));
}

/*
 * Free all the contents of a param container
 */
static void var_destructor(prte_mca_base_var_t *var)
{
    if ((PRTE_MCA_BASE_VAR_TYPE_STRING == var->mbv_type
         || PRTE_MCA_BASE_VAR_TYPE_VERSION_STRING == var->mbv_type)
        && NULL != var->mbv_storage && NULL != var->mbv_storage->stringval) {
        free(var->mbv_storage->stringval);
        var->mbv_storage->stringval = NULL;
    }

    /* don't release the boolean enumerator */
    PRTE_MCA_VAR_MBV_ENUMERATOR_FREE(var->mbv_enumerator);

    if (NULL != var->mbv_long_name) {
        free(var->mbv_long_name);
    }
    var->mbv_full_name = NULL;
    var->mbv_variable_name = NULL;

    if (NULL != var->mbv_description) {
        free(var->mbv_description);
    }

    /* Destroy the synonym array */
    PRTE_DESTRUCT(&var->mbv_synonyms);

    /* mark this parameter as invalid */
    var->mbv_type = PRTE_MCA_BASE_VAR_TYPE_MAX;

#if PRTE_ENABLE_DEBUG
    /* Cheap trick to reset everything to NULL */
    memset((char *) var + sizeof(var->super), 0, sizeof(*var) - sizeof(var->super));
#endif
}

static void fv_constructor(prte_mca_base_var_file_value_t *f)
{
    memset((char *) f + sizeof(f->super), 0, sizeof(*f) - sizeof(f->super));
}

static void fv_destructor(prte_mca_base_var_file_value_t *f)
{
    if (NULL != f->mbvfv_var) {
        free(f->mbvfv_var);
    }
    if (NULL != f->mbvfv_value) {
        free(f->mbvfv_value);
    }
    /* the file name is stored in mca_*/
    fv_constructor(f);
}

static char *source_name(prte_mca_base_var_t *var)
{
    char *ret;

    if (PRTE_MCA_BASE_VAR_SOURCE_FILE == var->mbv_source
        || PRTE_MCA_BASE_VAR_SOURCE_OVERRIDE == var->mbv_source) {
        struct prte_mca_base_var_file_value_t *fv = var->mbv_file_value;
        int rc;

        if (fv) {
            rc = prte_asprintf(&ret, "file (%s:%d)", fv->mbvfv_file, fv->mbvfv_lineno);
        } else {
            rc = prte_asprintf(&ret, "file (%s)", var->mbv_source_file);
        }

        /* some compilers will warn if the return code of prte_asprintf is not checked (even if it
         * is cast to void) */
        if (0 > rc) {
            return NULL;
        }
        return ret;
    } else if (PRTE_MCA_BASE_VAR_SOURCE_MAX <= var->mbv_source) {
        return strdup("unknown(!!)");
    }

    return strdup(prte_var_source_names[var->mbv_source]);
}

static int var_value_string(prte_mca_base_var_t *var, char **value_string)
{
    const prte_mca_base_var_storage_t *value = NULL;
    int ret;

    assert(PRTE_MCA_BASE_VAR_TYPE_MAX > var->mbv_type);

    /** Parameters with MCA_BASE_VAR_FLAG_DEF_UNSET flag should be shown
     * as "unset" by default. */
    if ((var->mbv_flags & PRTE_MCA_BASE_VAR_FLAG_DEF_UNSET)
        && (PRTE_MCA_BASE_VAR_SOURCE_DEFAULT == var->mbv_source)) {
        prte_asprintf(value_string, "%s", "unset");
        return PRTE_SUCCESS;
    }

    ret = prte_mca_base_var_get_value(var->mbv_index, &value, NULL, NULL);
    if (PRTE_SUCCESS != ret || NULL == value) {
        return ret;
    }

    if (NULL == var->mbv_enumerator) {
        switch (var->mbv_type) {
        case PRTE_MCA_BASE_VAR_TYPE_INT:
            ret = prte_asprintf(value_string, "%d", value->intval);
            break;
        case PRTE_MCA_BASE_VAR_TYPE_INT32_T:
            ret = prte_asprintf(value_string, "%" PRId32, value->int32tval);
            break;
        case PRTE_MCA_BASE_VAR_TYPE_UINT32_T:
            ret = prte_asprintf(value_string, "%" PRIu32, value->uint32tval);
            break;
        case PRTE_MCA_BASE_VAR_TYPE_INT64_T:
            ret = prte_asprintf(value_string, "%" PRId64, value->int64tval);
            break;
        case PRTE_MCA_BASE_VAR_TYPE_UINT64_T:
            ret = prte_asprintf(value_string, "%" PRIu64, value->uint64tval);
            break;
        case PRTE_MCA_BASE_VAR_TYPE_LONG:
            ret = prte_asprintf(value_string, "%ld", value->longval);
            break;
        case PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT:
            ret = prte_asprintf(value_string, "%u", value->uintval);
            break;
        case PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG:
            ret = prte_asprintf(value_string, "%lu", value->ulval);
            break;
        case PRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG_LONG:
            ret = prte_asprintf(value_string, "%llu", value->ullval);
            break;
        case PRTE_MCA_BASE_VAR_TYPE_SIZE_T:
            ret = prte_asprintf(value_string, "%" PRIsize_t, value->sizetval);
            break;
        case PRTE_MCA_BASE_VAR_TYPE_STRING:
        case PRTE_MCA_BASE_VAR_TYPE_VERSION_STRING:
            ret = prte_asprintf(value_string, "%s", value->stringval ? value->stringval : "");
            break;
        case PRTE_MCA_BASE_VAR_TYPE_BOOL:
            ret = prte_asprintf(value_string, "%d", value->boolval);
            break;
        case PRTE_MCA_BASE_VAR_TYPE_DOUBLE:
            ret = prte_asprintf(value_string, "%lf", value->lfval);
            break;
        default:
            ret = -1;
            break;
        }

        ret = (0 > ret) ? PRTE_ERR_OUT_OF_RESOURCE : PRTE_SUCCESS;
    } else {
        /* we use an enumerator to handle string->bool and bool->string conversion */
        if (PRTE_MCA_BASE_VAR_TYPE_BOOL == var->mbv_type) {
            ret = var->mbv_enumerator->string_from_value(var->mbv_enumerator, value->boolval,
                                                         value_string);
        } else {
            ret = var->mbv_enumerator->string_from_value(var->mbv_enumerator, value->intval,
                                                         value_string);
        }
    }

    return ret;
}

int prte_mca_base_var_check_exclusive(const char *project, const char *type_a,
                                      const char *component_a, const char *param_a,
                                      const char *type_b, const char *component_b,
                                      const char *param_b)
{
    prte_mca_base_var_t *var_a = NULL, *var_b = NULL;
    int var_ai, var_bi;

    /* XXX -- Remove me once the project name is in the componennt */
    project = NULL;

    var_ai = prte_mca_base_var_find(project, type_a, component_a, param_a);
    var_bi = prte_mca_base_var_find(project, type_b, component_b, param_b);
    if (var_bi < 0 || var_ai < 0) {
        return PRTE_ERR_NOT_FOUND;
    }

    (void) var_get(var_ai, &var_a, true);
    (void) var_get(var_bi, &var_b, true);
    if (NULL == var_a || NULL == var_b) {
        return PRTE_ERR_NOT_FOUND;
    }

    if (PRTE_MCA_BASE_VAR_SOURCE_DEFAULT != var_a->mbv_source
        && PRTE_MCA_BASE_VAR_SOURCE_DEFAULT != var_b->mbv_source) {
        char *str_a, *str_b;

        /* Form cosmetic string names for A */
        str_a = source_name(var_a);

        /* Form cosmetic string names for B */
        str_b = source_name(var_b);

        /* Print it all out */
        prte_show_help("help-prte-mca-var.txt", "mutually-exclusive-vars", true,
                       var_a->mbv_full_name, str_a, var_b->mbv_full_name, str_b);

        /* Free the temp strings */
        free(str_a);
        free(str_b);

        return PRTE_ERR_BAD_PARAM;
    }

    return PRTE_SUCCESS;
}

int prte_mca_base_var_get_count(void)
{
    return prte_mca_base_var_count;
}

int prte_mca_base_var_dump(int vari, char ***out, prte_mca_base_var_dump_type_t output_type)
{
    const char *framework, *component, *full_name;
    int i, line_count, line = 0, enum_count = 0;
    char *value_string, *source_string, *tmp;
    int synonym_count, ret, *synonyms = NULL;
    prte_mca_base_var_t *var, *original = NULL;
    prte_mca_base_var_group_t *group;

    ret = var_get(vari, &var, false);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    ret = prte_mca_base_var_group_get_internal(var->mbv_group_index, &group, false);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    if (PRTE_VAR_IS_SYNONYM(var[0])) {
        ret = var_get(var->mbv_synonym_for, &original, false);
        if (PRTE_SUCCESS != ret) {
            return ret;
        }
        /* just for protection... */
        if (NULL == original) {
            return PRTE_ERR_NOT_FOUND;
        }
    }

    framework = group->group_framework;
    component = group->group_component ? group->group_component : "base";
    full_name = var->mbv_full_name;

    synonym_count = prte_value_array_get_size(&var->mbv_synonyms);
    if (synonym_count) {
        synonyms = PRTE_VALUE_ARRAY_GET_BASE(&var->mbv_synonyms, int);
    }

    ret = var_value_string(var, &value_string);
    if (PRTE_SUCCESS != ret) {
        return ret;
    }

    source_string = source_name(var);
    if (NULL == source_string) {
        free(value_string);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    if (PRTE_MCA_BASE_VAR_DUMP_PARSABLE == output_type) {
        if (NULL != var->mbv_enumerator) {
            (void) var->mbv_enumerator->get_count(var->mbv_enumerator, &enum_count);
        }

        line_count = 8 + (var->mbv_description ? 1 : 0)
                     + (PRTE_VAR_IS_SYNONYM(var[0]) ? 1 : synonym_count) + enum_count;

        *out = (char **) calloc(line_count + 1, sizeof(char *));
        if (NULL == *out) {
            free(value_string);
            free(source_string);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        /* build the message*/
        prte_asprintf(&tmp, "mca:%s:%s:param:%s:", framework, component, full_name);

        /* Output the value */
        char *colon = strchr(value_string, ':');
        if (NULL != colon) {
            prte_asprintf(out[0] + line++, "%svalue:\"%s\"", tmp, value_string);
        } else {
            prte_asprintf(out[0] + line++, "%svalue:%s", tmp, value_string);
        }

        /* Output the source */
        prte_asprintf(out[0] + line++, "%ssource:%s", tmp, source_string);

        /* Output whether it's read only or writable */
        prte_asprintf(out[0] + line++, "%sstatus:%s", tmp,
                      PRTE_VAR_IS_SETTABLE(var[0]) ? "writeable" : "read-only");

        /* Output the info level of this parametere */
        prte_asprintf(out[0] + line++, "%slevel:%d", tmp, var->mbv_info_lvl + 1);

        /* If it has a help message, output the help message */
        if (var->mbv_description) {
            prte_asprintf(out[0] + line++, "%shelp:%s", tmp, var->mbv_description);
        }

        if (NULL != var->mbv_enumerator) {
            for (i = 0; i < enum_count; ++i) {
                const char *enum_string = NULL;
                int enum_value;

                ret = var->mbv_enumerator->get_value(var->mbv_enumerator, i, &enum_value,
                                                     &enum_string);
                if (PRTE_SUCCESS != ret) {
                    continue;
                }

                prte_asprintf(out[0] + line++, "%senumerator:value:%d:%s", tmp, enum_value,
                              enum_string);
            }
        }

        /* Is this variable deprecated? */
        prte_asprintf(out[0] + line++, "%sdeprecated:%s", tmp,
                      PRTE_VAR_IS_DEPRECATED(var[0]) ? "yes" : "no");

        prte_asprintf(out[0] + line++, "%stype:%s", tmp, prte_var_type_names[var->mbv_type]);

        /* Does this parameter have any synonyms or is it a synonym? */
        if (PRTE_VAR_IS_SYNONYM(var[0])) {
            prte_asprintf(out[0] + line++, "%ssynonym_of:name:%s", tmp, original->mbv_full_name);
        } else if (prte_value_array_get_size(&var->mbv_synonyms)) {
            for (i = 0; i < synonym_count; ++i) {
                prte_mca_base_var_t *synonym;

                ret = var_get(synonyms[i], &synonym, false);
                if (PRTE_SUCCESS != ret) {
                    continue;
                }

                prte_asprintf(out[0] + line++, "%ssynonym:name:%s", tmp, synonym->mbv_full_name);
            }
        }

        free(tmp);
    } else if (PRTE_MCA_BASE_VAR_DUMP_READABLE == output_type) {
        /* There will be at most three lines in the pretty print case */
        *out = (char **) calloc(4, sizeof(char *));
        if (NULL == *out) {
            free(value_string);
            free(source_string);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        prte_asprintf(out[0],
                      "%s \"%s\" (current value: \"%s\", data source: %s, level: %d %s, type: %s",
                      PRTE_VAR_IS_DEFAULT_ONLY(var[0]) ? "informational" : "parameter", full_name,
                      value_string, source_string, var->mbv_info_lvl + 1,
                      prte_info_lvl_strings[var->mbv_info_lvl], prte_var_type_names[var->mbv_type]);

        tmp = out[0][0];
        if (PRTE_VAR_IS_DEPRECATED(var[0])) {
            prte_asprintf(out[0], "%s, deprecated", tmp);
            free(tmp);
            tmp = out[0][0];
        }

        /* Does this parameter have any synonyms or is it a synonym? */
        if (PRTE_VAR_IS_SYNONYM(var[0])) {
            prte_asprintf(out[0], "%s, synonym of: %s)", tmp, original->mbv_full_name);
            free(tmp);
        } else if (synonym_count) {
            prte_asprintf(out[0], "%s, synonyms: ", tmp);
            free(tmp);

            for (i = 0; i < synonym_count; ++i) {
                prte_mca_base_var_t *synonym;

                ret = var_get(synonyms[i], &synonym, false);
                if (PRTE_SUCCESS != ret) {
                    continue;
                }

                tmp = out[0][0];
                if (synonym_count == i + 1) {
                    prte_asprintf(out[0], "%s%s)", tmp, synonym->mbv_full_name);
                } else {
                    prte_asprintf(out[0], "%s%s, ", tmp, synonym->mbv_full_name);
                }
                free(tmp);
            }
        } else {
            prte_asprintf(out[0], "%s)", tmp);
            free(tmp);
        }

        line++;

        if (var->mbv_description) {
            prte_asprintf(out[0] + line++, "%s", var->mbv_description);
        }

        if (NULL != var->mbv_enumerator) {
            char *values;

            ret = var->mbv_enumerator->dump(var->mbv_enumerator, &values);
            if (PRTE_SUCCESS == ret) {
                prte_asprintf(out[0] + line++, "Valid values: %s", values);
                free(values);
            }
        }
    } else if (PRTE_MCA_BASE_VAR_DUMP_SIMPLE == output_type) {
        *out = (char **) calloc(2, sizeof(char *));
        if (NULL == *out) {
            free(value_string);
            free(source_string);
            return PRTE_ERR_OUT_OF_RESOURCE;
        }

        prte_asprintf(out[0], "%s=%s (%s)", var->mbv_full_name, value_string, source_string);
    }

    free(value_string);
    free(source_string);

    return PRTE_SUCCESS;
}
