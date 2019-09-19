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
 * Copyright (c) 2008-2018 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2012-2018 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2018      Triad National Security, LLC. All rights
 *                         reserved.
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
#include "src/mca/installdirs/installdirs.h"
#include "src/util/os_path.h"
#include "src/util/path.h"
#include "src/util/show_help.h"
#include "src/util/printf.h"
#include "src/util/argv.h"
#include "src/mca/mca.h"
#include "src/mca/base/prrte_mca_base_vari.h"
#include "constants.h"
#include "src/util/output.h"
#include "src/util/prrte_environ.h"
#include "src/runtime/runtime.h"

/*
 * local variables
 */
static prrte_pointer_array_t prrte_mca_base_vars;
static const char *prrte_mca_prefix = PRRTE_MCA_PREFIX;
static char *home = NULL;
static char *cwd  = NULL;
bool prrte_mca_base_var_initialized = false;
static char * force_agg_path = NULL;
static char *prrte_mca_base_var_files = NULL;
static char *prrte_mca_base_envar_files = NULL;
static char **prrte_mca_base_var_file_list = NULL;
static char *prrte_mca_base_var_override_file = NULL;
static char *prrte_mca_base_var_file_prefix = NULL;
static char *prrte_mca_base_envar_file_prefix = NULL;
static char *prrte_mca_base_param_file_path = NULL;
char *prrte_mca_base_env_list = NULL;
char *prrte_mca_base_env_list_sep = PRRTE_MCA_BASE_ENV_LIST_SEP_DEFAULT;
char *prrte_mca_base_env_list_internal = NULL;
static bool prrte_mca_base_var_suppress_override_warning = false;
static prrte_list_t prrte_mca_base_var_file_values;
static prrte_list_t prrte_mca_base_envar_file_values;
static prrte_list_t prrte_mca_base_var_override_values;

static int prrte_mca_base_var_count = 0;

static prrte_hash_table_t prrte_mca_base_var_index_hash;

const char *prrte_var_type_names[] = {
    "int",
    "unsigned_int",
    "unsigned_long",
    "unsigned_long_long",
    "size_t",
    "string",
    "version_string",
    "bool",
    "double",
    "long",
    "int32_t",
    "uint32_t",
    "int64_t",
    "uint64_t",
};

const size_t prrte_var_type_sizes[] = {
    sizeof (int),
    sizeof (unsigned),
    sizeof (unsigned long),
    sizeof (unsigned long long),
    sizeof (size_t),
    sizeof (char),
    sizeof (char),
    sizeof (bool),
    sizeof (double),
    sizeof (long),
    sizeof (int32_t),
    sizeof (uint32_t),
    sizeof (int64_t),
    sizeof (uint64_t),
};

static const char *prrte_var_source_names[] = {
    "default",
    "command line",
    "environment",
    "file",
    "set",
    "override"
};


static const char *prrte_info_lvl_strings[] = {
    "user/basic",
    "user/detail",
    "user/all",
    "tuner/basic",
    "tuner/detail",
    "tuner/all",
    "dev/basic",
    "dev/detail",
    "dev/all"
};

/*
 * local functions
 */
static int fixup_files(char **file_list, char * path, bool rel_path_search, char sep);
static int read_files (char *file_list, prrte_list_t *file_values, char sep);
static int var_set_initial (prrte_mca_base_var_t *var, prrte_mca_base_var_t *original);
static int var_get (int vari, prrte_mca_base_var_t **var_out, bool original);
static int var_value_string (prrte_mca_base_var_t *var, char **value_string);

/*
 * classes
 */
static void var_constructor (prrte_mca_base_var_t *p);
static void var_destructor (prrte_mca_base_var_t *p);
PRRTE_CLASS_INSTANCE(prrte_mca_base_var_t, prrte_object_t,
                   var_constructor, var_destructor);

static void fv_constructor (prrte_mca_base_var_file_value_t *p);
static void fv_destructor (prrte_mca_base_var_file_value_t *p);
PRRTE_CLASS_INSTANCE(prrte_mca_base_var_file_value_t, prrte_list_item_t,
                   fv_constructor, fv_destructor);

static const char *prrte_mca_base_var_source_file (const prrte_mca_base_var_t *var)
{
    prrte_mca_base_var_file_value_t *fv = (prrte_mca_base_var_file_value_t *) var->mbv_file_value;

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
int prrte_mca_base_var_generate_full_name4 (const char *project, const char *framework, const char *component,
                                            const char *variable, char **full_name)
{
    const char * const names[] = {project, framework, component, variable};
    char *name, *tmp;
    size_t i, len;

    *full_name = NULL;

    for (i = 0, len = 0 ; i < 4 ; ++i) {
        if (NULL != names[i]) {
            /* Add space for the string + _ or \0 */
            len += strlen (names[i]) + 1;
        }
    }

    name = calloc (1, len);
    if (NULL == name) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0, tmp = name ; i < 4 ; ++i) {
        if (NULL != names[i]) {
            if (name != tmp) {
                *tmp++ = '_';
            }
            strncat (name, names[i], len - (size_t)(uintptr_t)(tmp - name));
            tmp += strlen (names[i]);
        }
    }

    *full_name = name;
    return PRRTE_SUCCESS;
}

static int compare_strings (const char *str1, const char *str2) {
    if ((NULL != str1 && 0 == strcmp (str1, "*")) ||
        (NULL == str1 && NULL == str2)) {
        return 0;
    }

    if (NULL != str1 && NULL != str2) {
        return strcmp (str1, str2);
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

    (void) prrte_argv_append_unique_nosize(&prrte_mca_base_var_file_list, filename, false);

    count = prrte_argv_count(prrte_mca_base_var_file_list);

    for (i = count - 1; i >= 0; --i) {
        if (0 == strcmp (prrte_mca_base_var_file_list[i], filename)) {
            return prrte_mca_base_var_file_list[i];
        }
    }

    /* *#@*? */
    return NULL;
}

/*
 * Set it up
 */
int prrte_mca_base_var_init(void)
{
    int ret;

    if (!prrte_mca_base_var_initialized) {
        /* Init the value array for the param storage */

        PRRTE_CONSTRUCT(&prrte_mca_base_vars, prrte_pointer_array_t);
        /* These values are arbitrary */
        ret = prrte_pointer_array_init (&prrte_mca_base_vars, 128, 16384, 128);
        if (PRRTE_SUCCESS != ret) {
            return ret;
        }

        prrte_mca_base_var_count = 0;

        /* Init the file param value list */

        PRRTE_CONSTRUCT(&prrte_mca_base_var_file_values, prrte_list_t);
        PRRTE_CONSTRUCT(&prrte_mca_base_envar_file_values, prrte_list_t);
        PRRTE_CONSTRUCT(&prrte_mca_base_var_override_values, prrte_list_t);
        PRRTE_CONSTRUCT(&prrte_mca_base_var_index_hash, prrte_hash_table_t);

        ret = prrte_hash_table_init (&prrte_mca_base_var_index_hash, 1024);
        if (PRRTE_SUCCESS != ret) {
            return ret;
        }

        ret = prrte_mca_base_var_group_init ();
        if  (PRRTE_SUCCESS != ret) {
            return ret;
        }

        /* Set this before we register the parameter, below */

        prrte_mca_base_var_initialized = true;

    }

    return PRRTE_SUCCESS;
}

static void process_env_list(char *env_list, char ***argv, char sep)
{
    char** tokens;
    char *ptr, *value;

    tokens = prrte_argv_split(env_list, (int)sep);
    if (NULL == tokens) {
        return;
    }

    for (int i = 0 ; NULL != tokens[i] ; ++i) {
        if (NULL == (ptr = strchr(tokens[i], '='))) {
            value = getenv(tokens[i]);
            if (NULL == value) {
                prrte_show_help("help-prrte-mca-var.txt", "incorrect-env-list-param",
                               true, tokens[i], env_list);
                break;
            }

            /* duplicate the value to silence tainted string coverity issue */
            value = strdup (value);
            if (NULL == value) {
                /* out of memory */
                break;
            }

            if (NULL != (ptr = strchr(value, '='))) {
                *ptr = '\0';
                prrte_setenv(value, ptr + 1, true, argv);
            } else {
                prrte_setenv(tokens[i], value, true, argv);
            }

            free (value);
        } else {
            *ptr = '\0';
            prrte_setenv(tokens[i], ptr + 1, true, argv);
            /* NTH: don't bother resetting ptr to = since the string will not be used again */
        }
    }

    prrte_argv_free(tokens);
}

int prrte_mca_base_var_process_env_list(char *list, char ***argv)
{
    char sep;
    sep = ';';
    if (NULL != prrte_mca_base_env_list_sep) {
        if (1 == strlen(prrte_mca_base_env_list_sep)) {
            sep = prrte_mca_base_env_list_sep[0];
        } else {
            prrte_show_help("help-prrte-mca-var.txt", "incorrect-env-list-sep",
                    true, prrte_mca_base_env_list_sep);
            return PRRTE_SUCCESS;
        }
    }
    if (NULL != list) {
        process_env_list(list, argv, sep);
    } else if (NULL != prrte_mca_base_env_list) {
        process_env_list(prrte_mca_base_env_list, argv, sep);
    }

    return PRRTE_SUCCESS;
}

int prrte_mca_base_var_process_env_list_from_file(char ***argv)
{
    if (NULL != prrte_mca_base_env_list_internal) {
        process_env_list(prrte_mca_base_env_list_internal, argv, ';');
    }
    return PRRTE_SUCCESS;
}

static void resolve_relative_paths(char **file_prefix, char *file_path, bool rel_path_search, char **files, char sep)
{
    char *tmp_str;
    /*
     * Resolve all relative paths.
     * the file list returned will contain only absolute paths
     */
    if( PRRTE_SUCCESS != fixup_files(file_prefix, file_path, rel_path_search, sep) ) {
#if 0
        /* JJH We need to die! */
        abort();
#else
        ;
#endif
    }
    else {
        /* Prepend the files to the search list */
        prrte_asprintf(&tmp_str, "%s%c%s", *file_prefix, sep, *files);
        free (*files);
        *files = tmp_str;
    }
}

int prrte_mca_base_var_cache_files(bool rel_path_search)
{
    char *tmp;
    int ret;

    /* We may need this later */
    home = (char*)prrte_home_directory();

    if (NULL == cwd) {
        cwd = (char *) malloc(sizeof(char) * MAXPATHLEN);
        if( NULL == (cwd = getcwd(cwd, MAXPATHLEN) )) {
            prrte_output(0, "Error: Unable to get the current working directory\n");
            cwd = strdup(".");
        }
    }

#if PRRTE_WANT_HOME_CONFIG_FILES
    prrte_asprintf(&prrte_mca_base_var_files, "%s"PRRTE_PATH_SEP".prrte" PRRTE_PATH_SEP
             "mca-params.conf%c%s" PRRTE_PATH_SEP "prrte-mca-params.conf",
             home, ',', prrte_install_dirs.sysconfdir);
#else
    prrte_asprintf(&prrte_mca_base_var_files, "%s" PRRTE_PATH_SEP "prrte-mca-params.conf",
             prrte_install_dirs.sysconfdir);
#endif

    /* Initialize a parameter that says where MCA param files can be found.
       We may change this value so set the scope to MCA_BASE_VAR_SCOPE_READONLY */
    tmp = prrte_mca_base_var_files;
    ret = prrte_mca_base_var_register ("prrte", "mca", "base", "param_files", "Path for MCA "
                                 "configuration files containing variable values",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_2,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_mca_base_var_files);
    free (tmp);
    if (0 > ret) {
        return ret;
    }

    prrte_mca_base_envar_files = strdup(prrte_mca_base_var_files);

    ret = prrte_asprintf(&prrte_mca_base_var_override_file, "%s" PRRTE_PATH_SEP "prrte-mca-params-override.conf",
                   prrte_install_dirs.sysconfdir);
    if (0 > ret) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    tmp = prrte_mca_base_var_override_file;
    ret = prrte_mca_base_var_register ("prrte", "mca", "base", "override_param_file",
                                 "Variables set in this file will override any value set in"
                                 "the environment or another configuration file",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, PRRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY,
                                 PRRTE_INFO_LVL_2, PRRTE_MCA_BASE_VAR_SCOPE_CONSTANT,
                                 &prrte_mca_base_var_override_file);
    free (tmp);
    if (0 > ret) {
        return ret;
    }

    /* Disable reading MCA parameter files. */
    if (0 == strcmp (prrte_mca_base_var_files, "none")) {
        return PRRTE_SUCCESS;
    }

    prrte_mca_base_var_suppress_override_warning = false;
    ret = prrte_mca_base_var_register ("prrte", "mca", "base", "suppress_override_warning",
                                 "Suppress warnings when attempting to set an overridden value (default: false)",
                                 PRRTE_MCA_BASE_VAR_TYPE_BOOL, NULL, 0, 0, PRRTE_INFO_LVL_2,
                                 PRRTE_MCA_BASE_VAR_SCOPE_LOCAL, &prrte_mca_base_var_suppress_override_warning);
    if (0 > ret) {
        return ret;
    }

    /* Aggregate MCA parameter files
     * A prefix search path to look up aggregate MCA parameter file
     * requests that do not specify an absolute path
     */
    prrte_mca_base_var_file_prefix = NULL;
    ret = prrte_mca_base_var_register ("prrte", "mca", "base", "param_file_prefix",
                                 "Aggregate MCA parameter file sets",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_3,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_mca_base_var_file_prefix);
    if (0 > ret) {
        return ret;
    }

    prrte_mca_base_envar_file_prefix = NULL;
    ret = prrte_mca_base_var_register ("prrte", "mca", "base", "envar_file_prefix",
                                 "Aggregate MCA parameter file set for env variables",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_3,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_mca_base_envar_file_prefix);
    if (0 > ret) {
        return ret;
    }

    ret = prrte_asprintf(&prrte_mca_base_param_file_path, "%s" PRRTE_PATH_SEP "prrte-amca-param-sets%c%s",
                   prrte_install_dirs.prrtedatadir, PRRTE_ENV_SEP, cwd);
    if (0 > ret) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    tmp = prrte_mca_base_param_file_path;
    ret = prrte_mca_base_var_register ("prrte", "mca", "base", "param_file_path",
                                 "Aggregate MCA parameter Search path",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_3,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &prrte_mca_base_param_file_path);
    free (tmp);
    if (0 > ret) {
        return ret;
    }

    force_agg_path = NULL;
    ret = prrte_mca_base_var_register ("prrte", "mca", "base", "param_file_path_force",
                                 "Forced Aggregate MCA parameter Search path",
                                 PRRTE_MCA_BASE_VAR_TYPE_STRING, NULL, 0, 0, PRRTE_INFO_LVL_3,
                                 PRRTE_MCA_BASE_VAR_SCOPE_READONLY, &force_agg_path);
    if (0 > ret) {
        return ret;
    }

    if (NULL != force_agg_path) {
        if (NULL != prrte_mca_base_param_file_path) {
            char *tmp_str = prrte_mca_base_param_file_path;

            prrte_asprintf(&prrte_mca_base_param_file_path, "%s%c%s", force_agg_path, PRRTE_ENV_SEP, tmp_str);
            free(tmp_str);
        } else {
            prrte_mca_base_param_file_path = strdup(force_agg_path);
        }
    }

    if (NULL != prrte_mca_base_var_file_prefix) {
       resolve_relative_paths(&prrte_mca_base_var_file_prefix, prrte_mca_base_param_file_path, rel_path_search, &prrte_mca_base_var_files, PRRTE_ENV_SEP);
    }
    read_files (prrte_mca_base_var_files, &prrte_mca_base_var_file_values, ',');

    if (NULL != prrte_mca_base_envar_file_prefix) {
       resolve_relative_paths(&prrte_mca_base_envar_file_prefix, prrte_mca_base_param_file_path, rel_path_search, &prrte_mca_base_envar_files, ',');
    }
    read_files (prrte_mca_base_envar_files, &prrte_mca_base_envar_file_values, ',');

    if (0 == access(prrte_mca_base_var_override_file, F_OK)) {
        read_files (prrte_mca_base_var_override_file, &prrte_mca_base_var_override_values, PRRTE_ENV_SEP);
    }

    return PRRTE_SUCCESS;
}

/*
 * Look up an integer MCA parameter.
 */
int prrte_mca_base_var_get_value (int vari, const void *value,
                                  prrte_mca_base_var_source_t *source,
                                  const char **source_file)
{
    prrte_mca_base_var_t *var;
    void **tmp = (void **) value;
    int ret;

    ret = var_get (vari, &var, true);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    if (!PRRTE_VAR_IS_VALID(var[0])) {
        return PRRTE_ERR_NOT_FOUND;
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
        *source_file = prrte_mca_base_var_source_file (var);
    }

    return PRRTE_SUCCESS;
}

static int var_set_string (prrte_mca_base_var_t *var, char *value)
{
    char *tmp;
    int ret;

    if (NULL != var->mbv_storage->stringval) {
        free (var->mbv_storage->stringval);
    }

    var->mbv_storage->stringval = NULL;

    if (NULL == value || 0 == strlen (value)) {
        return PRRTE_SUCCESS;
    }

    /* Replace all instances of ~/ in a path-style string with the
       user's home directory. This may be handled by the enumerator
       in the future. */
    if (0 == strncmp (value, "~/", 2)) {
        if (NULL != home) {
            ret = prrte_asprintf (&value, "%s/%s", home, value + 2);
            if (0 > ret) {
                return PRRTE_ERROR;
            }
        } else {
            value = strdup (value + 2);
        }
    } else {
        value = strdup (value);
    }

    if (NULL == value) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    while (NULL != (tmp = strstr (value, ":~/"))) {
        tmp[0] = '\0';
        tmp += 3;

        ret = prrte_asprintf (&tmp, "%s:%s%s%s", value,
                        home ? home : "", home ? "/" : "", tmp);

        free (value);

        if (0 > ret) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        value = tmp;
    }

    var->mbv_storage->stringval = value;

    return PRRTE_SUCCESS;
}

static int int_from_string(const char *src, prrte_mca_base_var_enum_t *enumerator, uint64_t *value_out)
{
    uint64_t value;
    bool is_int;
    char *tmp;

    if (NULL == src || 0 == strlen (src)) {
        if (NULL == enumerator) {
            *value_out = 0;
        }

        return PRRTE_SUCCESS;
    }

    if (enumerator) {
        int int_val, ret;
        ret = enumerator->value_from_string(enumerator, src, &int_val);
        if (PRRTE_SUCCESS != ret) {
            return ret;
        }
        *value_out = (uint64_t) int_val;

        return PRRTE_SUCCESS;
    }

    /* Check for an integer value */
    value = strtoull (src, &tmp, 0);
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

    return PRRTE_SUCCESS;
}

static int var_set_from_string (prrte_mca_base_var_t *var, char *src)
{
    prrte_mca_base_var_storage_t *dst = var->mbv_storage;
    uint64_t int_value = 0;
    int ret;

    switch (var->mbv_type) {
    case PRRTE_MCA_BASE_VAR_TYPE_INT:
    case PRRTE_MCA_BASE_VAR_TYPE_INT32_T:
    case PRRTE_MCA_BASE_VAR_TYPE_UINT32_T:
    case PRRTE_MCA_BASE_VAR_TYPE_LONG:
    case PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT:
    case PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG:
    case PRRTE_MCA_BASE_VAR_TYPE_INT64_T:
    case PRRTE_MCA_BASE_VAR_TYPE_UINT64_T:
    case PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG_LONG:
    case PRRTE_MCA_BASE_VAR_TYPE_BOOL:
    case PRRTE_MCA_BASE_VAR_TYPE_SIZE_T:
        ret = int_from_string(src, var->mbv_enumerator, &int_value);
        if (PRRTE_SUCCESS != ret ||
            (PRRTE_MCA_BASE_VAR_TYPE_INT == var->mbv_type && ((int) int_value != (int64_t) int_value)) ||
            (PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT == var->mbv_type && ((unsigned int) int_value != int_value))) {
            if (var->mbv_enumerator) {
                char *valid_values;
                (void) var->mbv_enumerator->dump(var->mbv_enumerator, &valid_values);
                prrte_show_help("help-prrte-mca-var.txt", "invalid-value-enum",
                               true, var->mbv_full_name, src, valid_values);
                free(valid_values);
            } else {
                prrte_show_help("help-prrte-mca-var.txt", "invalid-value",
                               true, var->mbv_full_name, src);
            }

            return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
        }

        if (PRRTE_MCA_BASE_VAR_TYPE_INT == var->mbv_type ||
            PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT == var->mbv_type) {
            int *castme = (int*) var->mbv_storage;
            *castme = int_value;
        } else if (PRRTE_MCA_BASE_VAR_TYPE_INT32_T == var->mbv_type ||
            PRRTE_MCA_BASE_VAR_TYPE_UINT32_T == var->mbv_type) {
            int32_t *castme = (int32_t *) var->mbv_storage;
            *castme = int_value;
        } else if (PRRTE_MCA_BASE_VAR_TYPE_INT64_T == var->mbv_type ||
            PRRTE_MCA_BASE_VAR_TYPE_UINT64_T == var->mbv_type) {
            int64_t *castme = (int64_t *) var->mbv_storage;
            *castme = int_value;
        } else if (PRRTE_MCA_BASE_VAR_TYPE_LONG == var->mbv_type) {
            long *castme = (long*) var->mbv_storage;
            *castme = (long) int_value;
        } else if (PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG == var->mbv_type) {
            unsigned long *castme = (unsigned long*) var->mbv_storage;
            *castme = (unsigned long) int_value;
        } else if (PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG_LONG == var->mbv_type) {
            unsigned long long *castme = (unsigned long long*) var->mbv_storage;
            *castme = (unsigned long long) int_value;
        } else if (PRRTE_MCA_BASE_VAR_TYPE_SIZE_T == var->mbv_type) {
            size_t *castme = (size_t*) var->mbv_storage;
            *castme = (size_t) int_value;
        } else if (PRRTE_MCA_BASE_VAR_TYPE_BOOL == var->mbv_type) {
            bool *castme = (bool*) var->mbv_storage;
            *castme = !!int_value;
        }

        return ret;
    case PRRTE_MCA_BASE_VAR_TYPE_DOUBLE:
        dst->lfval = strtod (src, NULL);
        break;
    case PRRTE_MCA_BASE_VAR_TYPE_STRING:
    case PRRTE_MCA_BASE_VAR_TYPE_VERSION_STRING:
        var_set_string (var, src);
        break;
    case PRRTE_MCA_BASE_VAR_TYPE_MAX:
        return PRRTE_ERROR;
    }

    return PRRTE_SUCCESS;
}

/*
 * Set a variable
 */
int prrte_mca_base_var_set_value (int vari, const void *value, size_t size,
                                  prrte_mca_base_var_source_t source, const char *source_file)
{
    prrte_mca_base_var_t *var;
    int ret;

    ret = var_get (vari, &var, true);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    if (!PRRTE_VAR_IS_VALID(var[0])) {
        return PRRTE_ERR_BAD_PARAM;
    }

    if (!PRRTE_VAR_IS_SETTABLE(var[0])) {
        return PRRTE_ERR_PERM;
    }

    if (NULL != var->mbv_enumerator) {
        /* Validate */
        ret = var->mbv_enumerator->string_from_value(var->mbv_enumerator,
                                                     ((int *) value)[0], NULL);
        if (PRRTE_SUCCESS != ret) {
            return ret;
        }
    }

    if (PRRTE_MCA_BASE_VAR_TYPE_STRING != var->mbv_type && PRRTE_MCA_BASE_VAR_TYPE_VERSION_STRING != var->mbv_type) {
        memmove (var->mbv_storage, value, prrte_var_type_sizes[var->mbv_type]);
    } else {
        var_set_string (var, (char *) value);
    }

    var->mbv_source = source;

    if (PRRTE_MCA_BASE_VAR_SOURCE_FILE == source && NULL != source_file) {
        var->mbv_file_value = NULL;
        var->mbv_source_file = append_filename_to_list(source_file);
    }

    return PRRTE_SUCCESS;
}

/*
 * Deregister a parameter
 */
int prrte_mca_base_var_deregister(int vari)
{
    prrte_mca_base_var_t *var;
    int ret;

    ret = var_get (vari, &var, false);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    if (!PRRTE_VAR_IS_VALID(var[0])) {
        return PRRTE_ERR_BAD_PARAM;
    }

    /* Mark this parameter as invalid but keep its info in case this
       parameter is reregistered later */
    var->mbv_flags &= ~PRRTE_MCA_BASE_VAR_FLAG_VALID;

    /* Done deregistering synonym */
    if (PRRTE_MCA_BASE_VAR_FLAG_SYNONYM & var->mbv_flags) {
        return PRRTE_SUCCESS;
    }

    /* Release the current value if it is a string. */
    if ((PRRTE_MCA_BASE_VAR_TYPE_STRING == var->mbv_type || PRRTE_MCA_BASE_VAR_TYPE_VERSION_STRING == var->mbv_type) &&
        var->mbv_storage->stringval) {
        free (var->mbv_storage->stringval);
        var->mbv_storage->stringval = NULL;
    } else if (var->mbv_enumerator && !var->mbv_enumerator->enum_is_static) {
        PRRTE_RELEASE(var->mbv_enumerator);
    }

    var->mbv_enumerator = NULL;

    var->mbv_storage = NULL;

    return PRRTE_SUCCESS;
}

static int var_get (int vari, prrte_mca_base_var_t **var_out, bool original)
{
    prrte_mca_base_var_t *var;

    if (var_out) {
        *var_out = NULL;
    }

    /* Check for bozo cases */
    if (!prrte_mca_base_var_initialized) {
        return PRRTE_ERROR;
    }

    if (vari < 0) {
        return PRRTE_ERR_BAD_PARAM;
    }

    var = prrte_pointer_array_get_item (&prrte_mca_base_vars, vari);
    if (NULL == var) {
        return PRRTE_ERR_BAD_PARAM;
    }

    if (PRRTE_VAR_IS_SYNONYM(var[0]) && original) {
        return var_get(var->mbv_synonym_for, var_out, false);
    }

    if (var_out) {
        *var_out = var;
    }

    return PRRTE_SUCCESS;
}

int prrte_mca_base_var_env_name(const char *param_name,
                                char **env_name)
{
    int ret;

    assert (NULL != env_name);

    ret = prrte_asprintf(env_name, "%s%s", prrte_mca_prefix, param_name);
    if (0 > ret) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    return PRRTE_SUCCESS;
}

/*
 * Find the index for an MCA parameter based on its names.
 */
static int var_find_by_name (const char *full_name, int *vari, bool invalidok)
{
    prrte_mca_base_var_t *var = NULL;
    void *tmp;
    int rc;

    rc = prrte_hash_table_get_value_ptr (&prrte_mca_base_var_index_hash, full_name, strlen (full_name),
                                        &tmp);
    if (PRRTE_SUCCESS != rc) {
        return rc;
    }

    (void) var_get ((int)(uintptr_t) tmp, &var, false);

    if (invalidok || (var && PRRTE_VAR_IS_VALID(var[0]))) {
        *vari = (int)(uintptr_t) tmp;
        return PRRTE_SUCCESS;
    }

    return PRRTE_ERR_NOT_FOUND;
}

static int var_find (const char *project_name, const char *framework_name,
                     const char *component_name, const char *variable_name,
                     bool invalidok)
{
    char *full_name;
    int ret, vari;

    ret = prrte_mca_base_var_generate_full_name4 (NULL, framework_name, component_name,
                                                  variable_name, &full_name);
    if (PRRTE_SUCCESS != ret) {
        return PRRTE_ERROR;
    }

    ret = var_find_by_name(full_name, &vari, invalidok);

    /* NTH: should we verify the name components match? */

    free (full_name);

    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    return vari;
}

/*
 * Find the index for an MCA parameter based on its name components.
 */
int prrte_mca_base_var_find (const char *project_name, const char *framework_name,
                             const char *component_name, const char *variable_name)
{
    return var_find (project_name, framework_name, component_name, variable_name, false);
}

/*
 * Find the index for an MCA parameter based on full name.
 */
int prrte_mca_base_var_find_by_name (const char *full_name, int *vari)
{
    return var_find_by_name (full_name, vari, false);
}

int prrte_mca_base_var_set_flag (int vari, prrte_mca_base_var_flag_t flag, bool set)
{
    prrte_mca_base_var_t *var;
    int ret;

    ret = var_get (vari, &var, true);
    if (PRRTE_SUCCESS != ret || PRRTE_VAR_IS_SYNONYM(var[0])) {
        return PRRTE_ERR_BAD_PARAM;
    }

    var->mbv_flags = (var->mbv_flags & ~flag) | (set ? flag : 0);

    /* All done */
    return PRRTE_SUCCESS;
}

/*
 * Return info on a parameter at an index
 */
int prrte_mca_base_var_get (int vari, const prrte_mca_base_var_t **var)
{
    int ret;
    ret = var_get (vari, (prrte_mca_base_var_t **) var, false);

    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    if (!PRRTE_VAR_IS_VALID(*(var[0]))) {
        return PRRTE_ERR_NOT_FOUND;
    }

    return PRRTE_SUCCESS;
}

/*
 * Make an argv-style list of strings suitable for an environment
 */
int prrte_mca_base_var_build_env(char ***env, int *num_env, bool internal)
{
    prrte_mca_base_var_t *var;
    size_t i, len;
    int ret;

    /* Check for bozo cases */

    if (!prrte_mca_base_var_initialized) {
        return PRRTE_ERROR;
    }

    /* Iterate through all the registered parameters */

    len = prrte_pointer_array_get_size(&prrte_mca_base_vars);
    for (i = 0; i < len; ++i) {
        char *value_string;
        char *str = NULL;

        var = prrte_pointer_array_get_item (&prrte_mca_base_vars, i);
        if (NULL == var) {
            continue;
        }

        /* Don't output default values or internal variables (unless
           requested) */
        if (PRRTE_MCA_BASE_VAR_SOURCE_DEFAULT == var->mbv_source ||
            (!internal && PRRTE_VAR_IS_INTERNAL(var[0]))) {
            continue;
        }

        if ((PRRTE_MCA_BASE_VAR_TYPE_STRING == var->mbv_type || PRRTE_MCA_BASE_VAR_TYPE_VERSION_STRING == var->mbv_type) &&
            NULL == var->mbv_storage->stringval) {
            continue;
        }

        ret = var_value_string (var, &value_string);
        if (PRRTE_SUCCESS != ret) {
            goto cleanup;
        }

        ret = prrte_asprintf (&str, "%s%s=%s", prrte_mca_prefix, var->mbv_full_name,
                        value_string);
        free (value_string);
        if (0 > ret) {
            goto cleanup;
        }

        prrte_argv_append(num_env, env, str);
        free(str);

        switch (var->mbv_source) {
        case PRRTE_MCA_BASE_VAR_SOURCE_FILE:
        case PRRTE_MCA_BASE_VAR_SOURCE_OVERRIDE:
            prrte_asprintf (&str, "%sSOURCE_%s=FILE:%s", prrte_mca_prefix, var->mbv_full_name,
                      prrte_mca_base_var_source_file (var));
            break;
        case PRRTE_MCA_BASE_VAR_SOURCE_COMMAND_LINE:
            prrte_asprintf (&str, "%sSOURCE_%s=COMMAND_LINE", prrte_mca_prefix, var->mbv_full_name);
            break;
        case PRRTE_MCA_BASE_VAR_SOURCE_ENV:
        case PRRTE_MCA_BASE_VAR_SOURCE_SET:
        case PRRTE_MCA_BASE_VAR_SOURCE_DEFAULT:
            str = NULL;
            break;
        case PRRTE_MCA_BASE_VAR_SOURCE_MAX:
            goto cleanup;
        }

        if (NULL != str) {
            prrte_argv_append(num_env, env, str);
            free(str);
        }
    }

    /* All done */

    return PRRTE_SUCCESS;

    /* Error condition */

 cleanup:
    if (*num_env > 0) {
        prrte_argv_free(*env);
        *num_env = 0;
        *env = NULL;
    }
    return PRRTE_ERR_NOT_FOUND;
}

/*
 * Shut down the MCA parameter system (normally only invoked by the
 * MCA framework itself).
 */
void prrte_mca_base_var_finalize (void)
{
    prrte_object_t *object;
    prrte_list_item_t *item;
    int size, i;

    if (prrte_mca_base_var_initialized) {
        size = prrte_pointer_array_get_size(&prrte_mca_base_vars);
        for (i = 0 ; i < size ; ++i) {
            object = prrte_pointer_array_get_item (&prrte_mca_base_vars, i);
            if (NULL != object) {
                PRRTE_RELEASE(object);
            }
        }
        PRRTE_DESTRUCT(&prrte_mca_base_vars);

        while (NULL !=
               (item = prrte_list_remove_first(&prrte_mca_base_var_file_values))) {
            PRRTE_RELEASE(item);
        }
        PRRTE_DESTRUCT(&prrte_mca_base_var_file_values);

        while (NULL !=
               (item = prrte_list_remove_first(&prrte_mca_base_envar_file_values))) {
            PRRTE_RELEASE(item);
        }
        PRRTE_DESTRUCT(&prrte_mca_base_envar_file_values);

        while (NULL !=
               (item = prrte_list_remove_first(&prrte_mca_base_var_override_values))) {
            PRRTE_RELEASE(item);
        }
        PRRTE_DESTRUCT(&prrte_mca_base_var_override_values);

        if( NULL != cwd ) {
            free(cwd);
            cwd = NULL;
        }

        prrte_mca_base_var_initialized = false;
        prrte_mca_base_var_count = 0;

        if (NULL != prrte_mca_base_var_file_list) {
            prrte_argv_free(prrte_mca_base_var_file_list);
        }
        prrte_mca_base_var_file_list = NULL;

        (void) prrte_mca_base_var_group_finalize ();

        PRRTE_DESTRUCT(&prrte_mca_base_var_index_hash);

        free (prrte_mca_base_envar_files);
        prrte_mca_base_envar_files = NULL;
    }
}


/*************************************************************************/
static int fixup_files(char **file_list, char * path, bool rel_path_search, char sep) {
    int exit_status = PRRTE_SUCCESS;
    char **files = NULL;
    char **search_path = NULL;
    char * tmp_file = NULL;
    char **argv = NULL;
    char *rel_path;
    int mode = R_OK; /* The file exists, and we can read it */
    int count, i, argc = 0;

    search_path = prrte_argv_split(path, PRRTE_ENV_SEP);
    files = prrte_argv_split(*file_list, sep);
    count = prrte_argv_count(files);

    rel_path = force_agg_path ? force_agg_path : cwd;

    /* Read in reverse order, so we can preserve the original ordering */
    for (i = 0 ; i < count; ++i) {
        char *msg_path = path;
        if (prrte_path_is_absolute(files[i])) {
            /* Absolute paths preserved */
            tmp_file = prrte_path_access(files[i], NULL, mode);
        } else if (!rel_path_search && NULL != strchr(files[i], PRRTE_PATH_SEP[0])) {
            /* Resolve all relative paths:
             *  - If filename contains a "/" (e.g., "./foo" or "foo/bar")
             *    - look for it relative to cwd
             *    - if exists, use it
             *    - ow warn/error
             */
            msg_path = rel_path;
            tmp_file = prrte_path_access(files[i], rel_path, mode);
        } else {
            /* Resolve all relative paths:
             * - Use path resolution
             *    - if found and readable, use it
             *    - otherwise, warn/error
             */
            tmp_file = prrte_path_find (files[i], search_path, mode, NULL);
        }

        if (NULL == tmp_file) {
            prrte_show_help("help-prrte-mca-var.txt", "missing-param-file",
                           true, getpid(), files[i], msg_path);
            exit_status = PRRTE_ERROR;
            break;
        }

        prrte_argv_append(&argc, &argv, tmp_file);

        free(tmp_file);
        tmp_file = NULL;
    }

    if (PRRTE_SUCCESS == exit_status) {
        free(*file_list);
        *file_list = prrte_argv_join(argv, sep);
    }

    if( NULL != files ) {
        prrte_argv_free(files);
        files = NULL;
    }

    if( NULL != argv ) {
        prrte_argv_free(argv);
        argv = NULL;
    }

    if( NULL != search_path ) {
        prrte_argv_free(search_path);
        search_path = NULL;
    }

    return exit_status;
}

static int read_files(char *file_list, prrte_list_t *file_values, char sep)
{
    char **tmp = prrte_argv_split(file_list, sep);
    int i, count;

    if (!tmp) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    count = prrte_argv_count(tmp);

    /* Iterate through all the files passed in -- read them in reverse
       order so that we preserve unix/shell path-like semantics (i.e.,
       the entries farthest to the left get precedence) */

    for (i = count - 1; i >= 0; --i) {
        char *file_name = append_filename_to_list (tmp[i]);
        prrte_mca_base_parse_paramfile(file_name, file_values);
    }

    prrte_argv_free (tmp);

    prrte_mca_base_internal_env_store();

    return PRRTE_SUCCESS;
}

/******************************************************************************/
static int register_variable (const char *project_name, const char *framework_name,
                              const char *component_name, const char *variable_name,
                              const char *description, prrte_mca_base_var_type_t type,
                              prrte_mca_base_var_enum_t *enumerator, int bind,
                              prrte_mca_base_var_flag_t flags, prrte_mca_base_var_info_lvl_t info_lvl,
                              prrte_mca_base_var_scope_t scope, int synonym_for,
                              void *storage)
{
    int ret, var_index, group_index, tmp;
    prrte_mca_base_var_group_t *group;
    prrte_mca_base_var_t *var, *original = NULL;

    /* Developer error. Storage can not be NULL and type must exist */
    assert (((flags & PRRTE_MCA_BASE_VAR_FLAG_SYNONYM) || NULL != storage) && type >= 0 && type < PRRTE_MCA_BASE_VAR_TYPE_MAX);

    /* Developer error: check max length of strings */
    if (NULL != project_name &&
        strlen(project_name) > PRRTE_MCA_BASE_MAX_PROJECT_NAME_LEN) {
        return PRRTE_ERR_BAD_PARAM;
    }
    if (NULL != framework_name &&
        strlen(framework_name) > PRRTE_MCA_BASE_MAX_TYPE_NAME_LEN) {
        return PRRTE_ERR_BAD_PARAM;
    }
    if (NULL != component_name &&
        strlen(component_name) > PRRTE_MCA_BASE_MAX_COMPONENT_NAME_LEN) {
        return PRRTE_ERR_BAD_PARAM;
    }
    if (NULL != variable_name &&
        strlen(variable_name) > PRRTE_MCA_BASE_MAX_VARIABLE_NAME_LEN) {
        return PRRTE_ERR_BAD_PARAM;
    }

#if PRRTE_ENABLE_DEBUG
    /* Developer error: check for alignments */
    uintptr_t align = 0;
    switch (type) {
    case PRRTE_MCA_BASE_VAR_TYPE_INT:
    case PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT:
        align = PRRTE_ALIGNMENT_INT;
        break;
    case PRRTE_MCA_BASE_VAR_TYPE_INT32_T:
    case PRRTE_MCA_BASE_VAR_TYPE_UINT32_T:
        align = PRRTE_ALIGNMENT_INT32;
        break;
    case PRRTE_MCA_BASE_VAR_TYPE_INT64_T:
    case PRRTE_MCA_BASE_VAR_TYPE_UINT64_T:
        align = PRRTE_ALIGNMENT_INT64;
        break;
    case PRRTE_MCA_BASE_VAR_TYPE_LONG:
    case PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG:
        align = PRRTE_ALIGNMENT_LONG;
        break;
    case PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG_LONG:
        align = PRRTE_ALIGNMENT_LONG_LONG;
        break;
    case PRRTE_MCA_BASE_VAR_TYPE_SIZE_T:
        align = PRRTE_ALIGNMENT_SIZE_T;
        break;
    case PRRTE_MCA_BASE_VAR_TYPE_BOOL:
        align = PRRTE_ALIGNMENT_BOOL;
        break;
    case PRRTE_MCA_BASE_VAR_TYPE_DOUBLE:
        align = PRRTE_ALIGNMENT_DOUBLE;
        break;
    case PRRTE_MCA_BASE_VAR_TYPE_VERSION_STRING:
    case PRRTE_MCA_BASE_VAR_TYPE_STRING:
    default:
        align = 0;
        break;
    }

    if (0 != align) {
        assert(((uintptr_t) storage) % align == 0);
    }

    /* Also check to ensure that synonym_for>=0 when
       MCA_BCASE_VAR_FLAG_SYNONYM is specified */
    if (flags & PRRTE_MCA_BASE_VAR_FLAG_SYNONYM && synonym_for < 0) {
        assert((flags & PRRTE_MCA_BASE_VAR_FLAG_SYNONYM) && synonym_for >= 0);
    }
#endif

    if (flags & PRRTE_MCA_BASE_VAR_FLAG_SYNONYM) {
        original = prrte_pointer_array_get_item (&prrte_mca_base_vars, synonym_for);
        if (NULL == original) {
            /* Attempting to create a synonym for a non-existent variable. probably a
             * developer error. */
            assert (NULL != original);
            return PRRTE_ERR_NOT_FOUND;
        }
    }

    /* There are data holes in the var struct */
    PRRTE_DEBUG_ZERO(var);

    /* Initialize the array if it has never been initialized */
    if (!prrte_mca_base_var_initialized) {
        prrte_mca_base_var_init();
    }

    /* See if this entry is already in the array */
    var_index = var_find (project_name, framework_name, component_name, variable_name,
                          true);

    if (0 > var_index) {
        /* Create a new parameter entry */
        group_index = prrte_mca_base_var_group_register (project_name, framework_name, component_name,
                                                   NULL);
        if (-1 > group_index) {
            return group_index;
        }

        /* Read-only and constant variables can't be settable */
        if (scope < PRRTE_MCA_BASE_VAR_SCOPE_LOCAL || (flags & PRRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY)) {
            if ((flags & PRRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY) && (flags & PRRTE_MCA_BASE_VAR_FLAG_SETTABLE)) {
                prrte_show_help("help-prrte-mca-var.txt", "invalid-flag-combination",
                               true, "PRRTE_MCA_BASE_VAR_FLAG_DEFAULT_ONLY", "PRRTE_MCA_BASE_VAR_FLAG_SETTABLE");
                return PRRTE_ERROR;
            }

            /* Should we print a warning for other cases? */
            flags &= ~PRRTE_MCA_BASE_VAR_FLAG_SETTABLE;
        }

        var = PRRTE_NEW(prrte_mca_base_var_t);

        var->mbv_type        = type;
        var->mbv_flags       = flags;
        var->mbv_group_index = group_index;
        var->mbv_info_lvl    = info_lvl;
        var->mbv_scope       = scope;
        var->mbv_synonym_for = synonym_for;
        var->mbv_bind        = bind;

        if (NULL != description) {
            var->mbv_description = strdup(description);
        }

        ret = prrte_mca_base_var_generate_full_name4 (project_name, framework_name, component_name,
                                                      variable_name, &var->mbv_long_name);
        if (PRRTE_SUCCESS != ret) {
            PRRTE_RELEASE(var);
            return PRRTE_ERROR;
        }
        /* The mbv_full_name and the variable name are subset of the mbv_long_name
         * so instead of allocating them we can just point into the var mbv_long_name
         * at the right location.
         */
        var->mbv_full_name = var->mbv_long_name +
                             (NULL == project_name ? 0 : (strlen(project_name)+1)); /* 1 for _ */
        if( NULL != variable_name ) {
            var->mbv_variable_name = var->mbv_full_name +
                                     (NULL == framework_name ? 0 : (strlen(framework_name)+1)) +
                                     (NULL == component_name ? 0 : (strlen(component_name)+1));
        }

        /* Add it to the array.  Note that we copy the mca_var_t by value,
           so the entire contents of the struct is copied.  The synonym list
           will always be empty at this point, so there's no need for an
           extra RETAIN or RELEASE. */
        var_index = prrte_pointer_array_add (&prrte_mca_base_vars, var);
        if (0 > var_index) {
            PRRTE_RELEASE(var);
            return PRRTE_ERROR;
        }

        var->mbv_index = var_index;

        if (0 <= group_index) {
            prrte_mca_base_var_group_add_var (group_index, var_index);
        }

        prrte_mca_base_var_count++;
        if (0 <= var_find_by_name (var->mbv_full_name, &tmp, 0)) {
            /* XXX --- FIXME: variable overshadows an existing variable. this is difficult to support */
            assert (0);
        }

        prrte_hash_table_set_value_ptr (&prrte_mca_base_var_index_hash, var->mbv_full_name, strlen (var->mbv_full_name),
                                       (void *)(uintptr_t) var_index);
    } else {
        ret = var_get (var_index, &var, false);
        if (PRRTE_SUCCESS != ret) {
            /* Shouldn't ever happen */
            return PRRTE_ERROR;
        }

        ret = prrte_mca_base_var_group_get_internal (var->mbv_group_index, &group, true);
        if (PRRTE_SUCCESS != ret) {
            /* Shouldn't ever happen */
            return PRRTE_ERROR;
        }

        if (!group->group_isvalid) {
            group->group_isvalid = true;
        }

        /* Verify the name components match */
        if (0 != compare_strings(framework_name, group->group_framework) ||
            0 != compare_strings(component_name, group->group_component) ||
            0 != compare_strings(variable_name, var->mbv_variable_name)) {
            prrte_show_help("help-prrte-mca-var.txt", "var-name-conflict",
                           true, var->mbv_full_name, framework_name,
                           component_name, variable_name,
                           group->group_framework, group->group_component,
                           var->mbv_variable_name);
            /* This is developer error. abort! */
            assert (0);
            return PRRTE_ERROR;
        }

        if (var->mbv_type != type) {
#if PRRTE_ENABLE_DEBUG
            prrte_show_help("help-prrte-mca-var.txt",
                           "re-register-with-different-type",
                           true, var->mbv_full_name);
#endif
            return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
        }
    }

    if (PRRTE_MCA_BASE_VAR_TYPE_BOOL == var->mbv_type) {
        enumerator = &prrte_mca_base_var_enum_bool;
    } else if (NULL != enumerator) {
        if (var->mbv_enumerator) {
            PRRTE_RELEASE (var->mbv_enumerator);
        }

        if (!enumerator->enum_is_static) {
            PRRTE_RETAIN(enumerator);
        }
    }

    var->mbv_enumerator = enumerator;

    if (!original) {
        var->mbv_storage = storage;

        /* make a copy of the default string value */
        if ((PRRTE_MCA_BASE_VAR_TYPE_STRING == type || PRRTE_MCA_BASE_VAR_TYPE_VERSION_STRING == type) && NULL != ((char **)storage)[0]) {
            ((char **)storage)[0] = strdup (((char **)storage)[0]);
        }
    } else {
        /* synonym variable */
        prrte_value_array_append_item(&original->mbv_synonyms, &var_index);
    }

    /* go ahead and mark this variable as valid */
    var->mbv_flags |= PRRTE_MCA_BASE_VAR_FLAG_VALID;

    ret = var_set_initial (var, original);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    /* All done */
    return var_index;
}

int prrte_mca_base_var_register (const char *project_name, const char *framework_name,
                                 const char *component_name, const char *variable_name,
                                 const char *description, prrte_mca_base_var_type_t type,
                                 prrte_mca_base_var_enum_t *enumerator, int bind,
                                 prrte_mca_base_var_flag_t flags,
                                 prrte_mca_base_var_info_lvl_t info_lvl,
                                 prrte_mca_base_var_scope_t scope, void *storage)
{
    /* Only integer variables can have enumerator */
    assert (NULL == enumerator || (PRRTE_MCA_BASE_VAR_TYPE_INT == type || PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT == type));

    return register_variable (project_name, framework_name, component_name,
                              variable_name, description, type, enumerator,
                              bind, flags, info_lvl, scope, -1, storage);
}

int prrte_mca_base_component_var_register (const prrte_mca_base_component_t *component,
                                           const char *variable_name, const char *description,
                                           prrte_mca_base_var_type_t type, prrte_mca_base_var_enum_t *enumerator,
                                           int bind, prrte_mca_base_var_flag_t flags,
                                           prrte_mca_base_var_info_lvl_t info_lvl,
                                           prrte_mca_base_var_scope_t scope, void *storage)
{
    return prrte_mca_base_var_register (component->mca_project_name, component->mca_type_name,
                                        component->mca_component_name,
                                        variable_name, description, type, enumerator,
                                        bind, flags | PRRTE_MCA_BASE_VAR_FLAG_DWG,
                                        info_lvl, scope, storage);
}

int prrte_mca_base_framework_var_register (const prrte_mca_base_framework_t *framework,
                                           const char *variable_name,
                                           const char *help_msg, prrte_mca_base_var_type_t type,
                                           prrte_mca_base_var_enum_t *enumerator, int bind,
                                           prrte_mca_base_var_flag_t flags,
                                           prrte_mca_base_var_info_lvl_t info_level,
                                           prrte_mca_base_var_scope_t scope, void *storage)
{
    return prrte_mca_base_var_register (framework->framework_project, framework->framework_name,
                                        "base", variable_name, help_msg, type, enumerator, bind,
                                        flags | PRRTE_MCA_BASE_VAR_FLAG_DWG, info_level, scope, storage);
}

int prrte_mca_base_var_register_synonym (int synonym_for, const char *project_name,
                                         const char *framework_name,
                                         const char *component_name,
                                         const char *synonym_name,
                                         prrte_mca_base_var_syn_flag_t flags)
{
    prrte_mca_base_var_flag_t var_flags = (prrte_mca_base_var_flag_t) PRRTE_MCA_BASE_VAR_FLAG_SYNONYM;
    prrte_mca_base_var_t *var;
    int ret;

    ret = var_get (synonym_for, &var, false);
    if (PRRTE_SUCCESS != ret || PRRTE_VAR_IS_SYNONYM(var[0])) {
        return PRRTE_ERR_BAD_PARAM;
    }

    if (flags & PRRTE_MCA_BASE_VAR_SYN_FLAG_DEPRECATED) {
        var_flags |= PRRTE_MCA_BASE_VAR_FLAG_DEPRECATED;
    }
    if (flags & PRRTE_MCA_BASE_VAR_SYN_FLAG_INTERNAL) {
        var_flags |= PRRTE_MCA_BASE_VAR_FLAG_INTERNAL;
    }

    return register_variable (project_name, framework_name, component_name,
                              synonym_name, var->mbv_description, var->mbv_type, var->mbv_enumerator,
                              var->mbv_bind, var_flags, var->mbv_info_lvl, var->mbv_scope,
                              synonym_for, NULL);
}

static int var_get_env (prrte_mca_base_var_t *var, const char *name, char **source, char **value)
{
    const char source_prefix[] = "SOURCE_";
    const int max_len = strlen(prrte_mca_prefix) + strlen(source_prefix) +
        strlen(name) + 1;
    char *envvar = alloca(max_len);
    if (NULL == envvar) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    int ret;
    ret = snprintf(envvar, max_len, "%s%s", prrte_mca_prefix, name);
    if (0 > ret) {
        return PRRTE_ERROR;
    }
    *value = getenv(envvar);
    if( NULL == *value ) {
        *source = NULL;
        return PRRTE_ERR_NOT_FOUND;
    }

    ret = snprintf(envvar, max_len, "%s%s%s", prrte_mca_prefix,
                   source_prefix, name);
    if( 0 > ret ) {
        return PRRTE_ERROR;
    }
    *source = getenv(envvar);

    return PRRTE_SUCCESS;
}

/*
 * Lookup a param in the environment
 */
static int var_set_from_env (prrte_mca_base_var_t *var, prrte_mca_base_var_t *original)
{
    const char *var_full_name = var->mbv_full_name;
    const char *var_long_name = var->mbv_long_name;
    bool deprecated = PRRTE_VAR_IS_DEPRECATED(var[0]);
    bool is_synonym = PRRTE_VAR_IS_SYNONYM(var[0]);
    char *source_env, *value_env;
    int ret;

    ret = var_get_env (var, var_long_name, &source_env, &value_env);
    if (PRRTE_SUCCESS != ret) {
        ret = var_get_env (var, var_full_name, &source_env, &value_env);
    }

    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    /* we found an environment variable but this variable is default-only. print
       a warning. */
    if (PRRTE_VAR_IS_DEFAULT_ONLY(original[0])) {
        prrte_show_help("help-prrte-mca-var.txt", "default-only-param-set",
                       true, var_full_name);

        return PRRTE_ERR_NOT_FOUND;
    }

    if (PRRTE_MCA_BASE_VAR_SOURCE_OVERRIDE == original->mbv_source) {
        if (!prrte_mca_base_var_suppress_override_warning) {
            prrte_show_help("help-prrte-mca-var.txt", "overridden-param-set",
                           true, var_full_name);
        }

        return PRRTE_ERR_NOT_FOUND;
    }

    original->mbv_source = PRRTE_MCA_BASE_VAR_SOURCE_ENV;

    if (NULL != source_env) {
        if (0 == strncasecmp (source_env, "file:", 5)) {
            original->mbv_source_file = append_filename_to_list(source_env + 5);
            if (0 == strcmp (var->mbv_source_file, prrte_mca_base_var_override_file)) {
                original->mbv_source = PRRTE_MCA_BASE_VAR_SOURCE_OVERRIDE;
            } else {
                original->mbv_source = PRRTE_MCA_BASE_VAR_SOURCE_FILE;
            }
        } else if (0 == strcasecmp (source_env, "command")) {
            var->mbv_source = PRRTE_MCA_BASE_VAR_SOURCE_COMMAND_LINE;
        }
    }

    if (deprecated) {
        const char *new_variable = "None (going away)";

        if (is_synonym) {
            new_variable = original->mbv_full_name;
        }

        switch (var->mbv_source) {
        case PRRTE_MCA_BASE_VAR_SOURCE_ENV:
            prrte_show_help("help-prrte-mca-var.txt", "deprecated-mca-env",
                           true, var_full_name, new_variable);
            break;
        case PRRTE_MCA_BASE_VAR_SOURCE_COMMAND_LINE:
            prrte_show_help("help-prrte-mca-var.txt", "deprecated-mca-cli",
                           true, var_full_name, new_variable);
            break;
        case PRRTE_MCA_BASE_VAR_SOURCE_FILE:
        case PRRTE_MCA_BASE_VAR_SOURCE_OVERRIDE:
            prrte_show_help("help-prrte-mca-var.txt", "deprecated-mca-file",
                           true, var_full_name, prrte_mca_base_var_source_file (var),
                           new_variable);
            break;

        case PRRTE_MCA_BASE_VAR_SOURCE_DEFAULT:
        case PRRTE_MCA_BASE_VAR_SOURCE_MAX:
        case PRRTE_MCA_BASE_VAR_SOURCE_SET:
            /* silence compiler warnings about unhandled enumerations */
            break;
        }
    }

    return var_set_from_string (original, value_env);
}

/*
 * Lookup a param in the files
 */
static int var_set_from_file (prrte_mca_base_var_t *var, prrte_mca_base_var_t *original, prrte_list_t *file_values)
{
    const char *var_full_name = var->mbv_full_name;
    const char *var_long_name = var->mbv_long_name;
    bool deprecated = PRRTE_VAR_IS_DEPRECATED(var[0]);
    bool is_synonym = PRRTE_VAR_IS_SYNONYM(var[0]);
    prrte_mca_base_var_file_value_t *fv;

    /* Scan through the list of values read in from files and try to
       find a match.  If we do, cache it on the param (for future
       lookups) and save it in the storage. */

    PRRTE_LIST_FOREACH(fv, file_values, prrte_mca_base_var_file_value_t) {
        if (0 != strcmp(fv->mbvfv_var, var_full_name) &&
            0 != strcmp(fv->mbvfv_var, var_long_name)) {
            continue;
        }

        /* found it */
        if (PRRTE_VAR_IS_DEFAULT_ONLY(var[0])) {
            prrte_show_help("help-prrte-mca-var.txt", "default-only-param-set",
                           true, var_full_name);

            return PRRTE_ERR_NOT_FOUND;
        }

        if (PRRTE_MCA_BASE_VAR_FLAG_ENVIRONMENT_ONLY & original->mbv_flags) {
            prrte_show_help("help-prrte-mca-var.txt", "environment-only-param",
                           true, var_full_name, fv->mbvfv_value,
                           fv->mbvfv_file);

            return PRRTE_ERR_NOT_FOUND;
        }

        if (PRRTE_MCA_BASE_VAR_SOURCE_OVERRIDE == original->mbv_source) {
            if (!prrte_mca_base_var_suppress_override_warning) {
                prrte_show_help("help-prrte-mca-var.txt", "overridden-param-set",
                               true, var_full_name);
            }

            return PRRTE_ERR_NOT_FOUND;
        }

        if (deprecated) {
            const char *new_variable = "None (going away)";

            if (is_synonym) {
                new_variable = original->mbv_full_name;
            }

            prrte_show_help("help-prrte-mca-var.txt", "deprecated-mca-file",
                           true, var_full_name, fv->mbvfv_file,
                           new_variable);
        }

        original->mbv_file_value = (void *) fv;
        original->mbv_source = PRRTE_MCA_BASE_VAR_SOURCE_FILE;
        if (is_synonym) {
            var->mbv_file_value = (void *) fv;
            var->mbv_source = PRRTE_MCA_BASE_VAR_SOURCE_FILE;
        }

        return var_set_from_string (original, fv->mbvfv_value);
    }

    return PRRTE_ERR_NOT_FOUND;
}

/*
 * Lookup the initial value for a parameter
 */
static int var_set_initial (prrte_mca_base_var_t *var, prrte_mca_base_var_t *original)
{
    int ret;

    if (original) {
        /* synonym */
        var->mbv_source = original->mbv_source;
        var->mbv_file_value = original->mbv_file_value;
        var->mbv_source_file = original->mbv_source_file;
    } else {
        var->mbv_source = PRRTE_MCA_BASE_VAR_SOURCE_DEFAULT;
        original = var;
    }

    /* Check all the places that the param may be hiding, in priority
       order. If the default only flag is set the user will get a
       warning if they try to set a value from the environment or a
       file. */
    ret = var_set_from_file (var, original, &prrte_mca_base_var_override_values);
    if (PRRTE_SUCCESS == ret) {
        var->mbv_flags = ~PRRTE_MCA_BASE_VAR_FLAG_SETTABLE & (var->mbv_flags | PRRTE_MCA_BASE_VAR_FLAG_OVERRIDE);
        var->mbv_source = PRRTE_MCA_BASE_VAR_SOURCE_OVERRIDE;
    }

    ret = var_set_from_env (var, original);
    if (PRRTE_ERR_NOT_FOUND != ret) {
        return ret;
    }

    ret = var_set_from_file (var, original, &prrte_mca_base_envar_file_values);
    if (PRRTE_ERR_NOT_FOUND != ret) {
        return ret;
    }

    ret = var_set_from_file (var, original, &prrte_mca_base_var_file_values);
    if (PRRTE_ERR_NOT_FOUND != ret) {
        return ret;
    }

    return PRRTE_SUCCESS;
}

/*
 * Create an empty param container
 */
static void var_constructor(prrte_mca_base_var_t *var)
{
    memset ((char *) var + sizeof (var->super), 0, sizeof (*var) - sizeof (var->super));

    var->mbv_type = PRRTE_MCA_BASE_VAR_TYPE_MAX;
    PRRTE_CONSTRUCT(&var->mbv_synonyms, prrte_value_array_t);
    prrte_value_array_init (&var->mbv_synonyms, sizeof (int));
}


/*
 * Free all the contents of a param container
 */
static void var_destructor(prrte_mca_base_var_t *var)
{
    if ((PRRTE_MCA_BASE_VAR_TYPE_STRING == var->mbv_type ||
         PRRTE_MCA_BASE_VAR_TYPE_VERSION_STRING == var->mbv_type) &&
        NULL != var->mbv_storage &&
        NULL != var->mbv_storage->stringval) {
        free (var->mbv_storage->stringval);
        var->mbv_storage->stringval = NULL;
    }

    /* don't release the boolean enumerator */
    if (var->mbv_enumerator && !var->mbv_enumerator->enum_is_static) {
        PRRTE_RELEASE(var->mbv_enumerator);
    }

    if (NULL != var->mbv_long_name) {
        free(var->mbv_long_name);
    }
    var->mbv_full_name = NULL;
    var->mbv_variable_name = NULL;

    if (NULL != var->mbv_description) {
        free(var->mbv_description);
    }

    /* Destroy the synonym array */
    PRRTE_DESTRUCT(&var->mbv_synonyms);

    /* mark this parameter as invalid */
    var->mbv_type = PRRTE_MCA_BASE_VAR_TYPE_MAX;

#if PRRTE_ENABLE_DEBUG
    /* Cheap trick to reset everything to NULL */
    memset ((char *) var + sizeof (var->super), 0, sizeof (*var) - sizeof (var->super));
#endif
}


static void fv_constructor(prrte_mca_base_var_file_value_t *f)
{
    memset ((char *) f + sizeof (f->super), 0, sizeof (*f) - sizeof (f->super));
}


static void fv_destructor(prrte_mca_base_var_file_value_t *f)
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

static char *source_name(prrte_mca_base_var_t *var)
{
    char *ret;

    if (PRRTE_MCA_BASE_VAR_SOURCE_FILE == var->mbv_source || PRRTE_MCA_BASE_VAR_SOURCE_OVERRIDE == var->mbv_source) {
        struct prrte_mca_base_var_file_value_t *fv = var->mbv_file_value;
        int rc;

        if (fv) {
            rc = prrte_asprintf(&ret, "file (%s:%d)", fv->mbvfv_file, fv->mbvfv_lineno);
        } else {
            rc = prrte_asprintf(&ret, "file (%s)", var->mbv_source_file);
        }

        /* some compilers will warn if the return code of prrte_asprintf is not checked (even if it is cast to void) */
        if (0 > rc) {
            return NULL;
        }
        return ret;
    } else if (PRRTE_MCA_BASE_VAR_SOURCE_MAX <= var->mbv_source) {
        return strdup ("unknown(!!)");
    }

    return strdup (prrte_var_source_names[var->mbv_source]);
}

static int var_value_string (prrte_mca_base_var_t *var, char **value_string)
{
    const prrte_mca_base_var_storage_t *value=NULL;
    int ret;

    assert (PRRTE_MCA_BASE_VAR_TYPE_MAX > var->mbv_type);

    /** Parameters with MCA_BASE_VAR_FLAG_DEF_UNSET flag should be shown
     * as "unset" by default. */
    if ((var->mbv_flags & PRRTE_MCA_BASE_VAR_FLAG_DEF_UNSET) &&
        (PRRTE_MCA_BASE_VAR_SOURCE_DEFAULT == var->mbv_source)){
        prrte_asprintf (value_string, "%s", "unset");
        return PRRTE_SUCCESS;
    }

    ret = prrte_mca_base_var_get_value(var->mbv_index, &value, NULL, NULL);
    if (PRRTE_SUCCESS != ret || NULL == value) {
        return ret;
    }

    if (NULL == var->mbv_enumerator) {
        switch (var->mbv_type) {
        case PRRTE_MCA_BASE_VAR_TYPE_INT:
            ret = prrte_asprintf (value_string, "%d", value->intval);
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_INT32_T:
            ret = prrte_asprintf (value_string, "%" PRId32, value->int32tval);
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_UINT32_T:
            ret = prrte_asprintf (value_string, "%" PRIu32, value->uint32tval);
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_INT64_T:
            ret = prrte_asprintf (value_string, "%" PRId64, value->int64tval);
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_UINT64_T:
            ret = prrte_asprintf (value_string, "%" PRIu64, value->uint64tval);
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_LONG:
            ret = prrte_asprintf (value_string, "%ld", value->longval);
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_INT:
            ret = prrte_asprintf (value_string, "%u", value->uintval);
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG:
            ret = prrte_asprintf (value_string, "%lu", value->ulval);
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_UNSIGNED_LONG_LONG:
            ret = prrte_asprintf (value_string, "%llu", value->ullval);
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_SIZE_T:
            ret = prrte_asprintf (value_string, "%" PRIsize_t, value->sizetval);
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_STRING:
        case PRRTE_MCA_BASE_VAR_TYPE_VERSION_STRING:
            ret = prrte_asprintf (value_string, "%s",
                            value->stringval ? value->stringval : "");
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_BOOL:
            ret = prrte_asprintf (value_string, "%d", value->boolval);
            break;
        case PRRTE_MCA_BASE_VAR_TYPE_DOUBLE:
            ret = prrte_asprintf (value_string, "%lf", value->lfval);
            break;
        default:
            ret = -1;
            break;
        }

        ret = (0 > ret) ? PRRTE_ERR_OUT_OF_RESOURCE : PRRTE_SUCCESS;
    } else {
        /* we use an enumerator to handle string->bool and bool->string conversion */
        if (PRRTE_MCA_BASE_VAR_TYPE_BOOL == var->mbv_type) {
            ret = var->mbv_enumerator->string_from_value(var->mbv_enumerator, value->boolval, value_string);
        } else {
            ret = var->mbv_enumerator->string_from_value(var->mbv_enumerator, value->intval, value_string);
        }
    }

    return ret;
}

int prrte_mca_base_var_check_exclusive (const char *project,
                                        const char *type_a,
                                        const char *component_a,
                                        const char *param_a,
                                        const char *type_b,
                                        const char *component_b,
                                        const char *param_b)
{
    prrte_mca_base_var_t *var_a = NULL, *var_b = NULL;
    int var_ai, var_bi;

    /* XXX -- Remove me once the project name is in the componennt */
    project = NULL;

    var_ai = prrte_mca_base_var_find (project, type_a, component_a, param_a);
    var_bi = prrte_mca_base_var_find (project, type_b, component_b, param_b);
    if (var_bi < 0 || var_ai < 0) {
        return PRRTE_ERR_NOT_FOUND;
    }

    (void) var_get (var_ai, &var_a, true);
    (void) var_get (var_bi, &var_b, true);
    if (NULL == var_a || NULL == var_b) {
        return PRRTE_ERR_NOT_FOUND;
    }

    if (PRRTE_MCA_BASE_VAR_SOURCE_DEFAULT != var_a->mbv_source &&
        PRRTE_MCA_BASE_VAR_SOURCE_DEFAULT != var_b->mbv_source) {
        char *str_a, *str_b;

        /* Form cosmetic string names for A */
        str_a = source_name(var_a);

        /* Form cosmetic string names for B */
        str_b = source_name(var_b);

        /* Print it all out */
        prrte_show_help("help-prrte-mca-var.txt",
                       "mutually-exclusive-vars",
                       true, var_a->mbv_full_name,
                       str_a, var_b->mbv_full_name,
                       str_b);

        /* Free the temp strings */
        free(str_a);
        free(str_b);

        return PRRTE_ERR_BAD_PARAM;
    }

    return PRRTE_SUCCESS;
}

int prrte_mca_base_var_get_count (void)
{
    return prrte_mca_base_var_count;
}

int prrte_mca_base_var_dump(int vari, char ***out, prrte_mca_base_var_dump_type_t output_type)
{
    const char *framework, *component, *full_name;
    int i, line_count, line = 0, enum_count = 0;
    char *value_string, *source_string, *tmp;
    int synonym_count, ret, *synonyms = NULL;
    prrte_mca_base_var_t *var, *original=NULL;
    prrte_mca_base_var_group_t *group;

    ret = var_get(vari, &var, false);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    ret = prrte_mca_base_var_group_get_internal(var->mbv_group_index, &group, false);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    if (PRRTE_VAR_IS_SYNONYM(var[0])) {
        ret = var_get(var->mbv_synonym_for, &original, false);
        if (PRRTE_SUCCESS != ret) {
            return ret;
        }
        /* just for protection... */
        if (NULL == original) {
            return PRRTE_ERR_NOT_FOUND;
        }
    }

    framework = group->group_framework;
    component = group->group_component ? group->group_component : "base";
    full_name = var->mbv_full_name;

    synonym_count = prrte_value_array_get_size(&var->mbv_synonyms);
    if (synonym_count) {
        synonyms = PRRTE_VALUE_ARRAY_GET_BASE(&var->mbv_synonyms, int);
    }

    ret = var_value_string (var, &value_string);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    source_string = source_name(var);
    if (NULL == source_string) {
        free (value_string);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    if (PRRTE_MCA_BASE_VAR_DUMP_PARSABLE == output_type) {
        if (NULL != var->mbv_enumerator) {
            (void) var->mbv_enumerator->get_count(var->mbv_enumerator, &enum_count);
        }

        line_count = 8 + (var->mbv_description ? 1 : 0) + (PRRTE_VAR_IS_SYNONYM(var[0]) ? 1 : synonym_count) +
            enum_count;

        *out = (char **) calloc (line_count + 1, sizeof (char *));
        if (NULL == *out) {
            free (value_string);
            free (source_string);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        /* build the message*/
        prrte_asprintf(&tmp, "mca:%s:%s:param:%s:", framework, component,
                 full_name);

        /* Output the value */
        char *colon = strchr(value_string, ':');
        if (NULL != colon) {
            prrte_asprintf(out[0] + line++, "%svalue:\"%s\"", tmp, value_string);
        } else {
            prrte_asprintf(out[0] + line++, "%svalue:%s", tmp, value_string);
        }

        /* Output the source */
        prrte_asprintf(out[0] + line++, "%ssource:%s", tmp, source_string);

        /* Output whether it's read only or writable */
        prrte_asprintf(out[0] + line++, "%sstatus:%s", tmp,
                      PRRTE_VAR_IS_SETTABLE(var[0]) ? "writeable" : "read-only");

        /* Output the info level of this parametere */
        prrte_asprintf(out[0] + line++, "%slevel:%d", tmp, var->mbv_info_lvl + 1);

        /* If it has a help message, output the help message */
        if (var->mbv_description) {
            prrte_asprintf(out[0] + line++, "%shelp:%s", tmp, var->mbv_description);
        }

        if (NULL != var->mbv_enumerator) {
            for (i = 0 ; i < enum_count ; ++i) {
                const char *enum_string = NULL;
                int enum_value;

                ret = var->mbv_enumerator->get_value(var->mbv_enumerator, i, &enum_value,
                                                     &enum_string);
                if (PRRTE_SUCCESS != ret) {
                    continue;
                }

                prrte_asprintf(out[0] + line++, "%senumerator:value:%d:%s", tmp, enum_value, enum_string);
            }
        }

        /* Is this variable deprecated? */
        prrte_asprintf(out[0] + line++, "%sdeprecated:%s", tmp, PRRTE_VAR_IS_DEPRECATED(var[0]) ? "yes" : "no");

        prrte_asprintf(out[0] + line++, "%stype:%s", tmp, prrte_var_type_names[var->mbv_type]);

        /* Does this parameter have any synonyms or is it a synonym? */
        if (PRRTE_VAR_IS_SYNONYM(var[0])) {
            prrte_asprintf(out[0] + line++, "%ssynonym_of:name:%s", tmp, original->mbv_full_name);
        } else if (prrte_value_array_get_size(&var->mbv_synonyms)) {
            for (i = 0 ; i < synonym_count ; ++i) {
                prrte_mca_base_var_t *synonym;

                ret = var_get(synonyms[i], &synonym, false);
                if (PRRTE_SUCCESS != ret) {
                    continue;
                }

                prrte_asprintf(out[0] + line++, "%ssynonym:name:%s", tmp, synonym->mbv_full_name);
            }
        }

        free (tmp);
    } else if (PRRTE_MCA_BASE_VAR_DUMP_READABLE == output_type) {
        /* There will be at most three lines in the pretty print case */
        *out = (char **) calloc (4, sizeof (char *));
        if (NULL == *out) {
            free (value_string);
            free (source_string);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        prrte_asprintf (out[0], "%s \"%s\" (current value: \"%s\", data source: %s, level: %d %s, type: %s",
                  PRRTE_VAR_IS_DEFAULT_ONLY(var[0]) ? "informational" : "parameter",
                  full_name, value_string, source_string, var->mbv_info_lvl + 1,
                  prrte_info_lvl_strings[var->mbv_info_lvl], prrte_var_type_names[var->mbv_type]);

        tmp = out[0][0];
        if (PRRTE_VAR_IS_DEPRECATED(var[0])) {
            prrte_asprintf (out[0], "%s, deprecated", tmp);
            free (tmp);
            tmp = out[0][0];
        }

        /* Does this parameter have any synonyms or is it a synonym? */
        if (PRRTE_VAR_IS_SYNONYM(var[0])) {
            prrte_asprintf(out[0], "%s, synonym of: %s)", tmp, original->mbv_full_name);
            free (tmp);
        } else if (synonym_count) {
            prrte_asprintf(out[0], "%s, synonyms: ", tmp);
            free (tmp);

            for (i = 0 ; i < synonym_count ; ++i) {
                prrte_mca_base_var_t *synonym;

                ret = var_get(synonyms[i], &synonym, false);
                if (PRRTE_SUCCESS != ret) {
                    continue;
                }

                tmp = out[0][0];
                if (synonym_count == i+1) {
                    prrte_asprintf(out[0], "%s%s)", tmp, synonym->mbv_full_name);
                } else {
                    prrte_asprintf(out[0], "%s%s, ", tmp, synonym->mbv_full_name);
                }
                free(tmp);
            }
        } else {
            prrte_asprintf(out[0], "%s)", tmp);
            free(tmp);
        }

        line++;

        if (var->mbv_description) {
            prrte_asprintf(out[0] + line++, "%s", var->mbv_description);
        }

        if (NULL != var->mbv_enumerator) {
            char *values;

            ret = var->mbv_enumerator->dump(var->mbv_enumerator, &values);
            if (PRRTE_SUCCESS == ret) {
                prrte_asprintf (out[0] + line++, "Valid values: %s", values);
                free (values);
            }
        }
    } else if (PRRTE_MCA_BASE_VAR_DUMP_SIMPLE == output_type) {
        *out = (char **) calloc (2, sizeof (char *));
        if (NULL == *out) {
            free (value_string);
            free (source_string);
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        prrte_asprintf(out[0], "%s=%s (%s)", var->mbv_full_name, value_string, source_string);
    }

    free (value_string);
    free (source_string);

    return PRRTE_SUCCESS;
}

