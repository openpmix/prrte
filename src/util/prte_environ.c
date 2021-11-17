/*
 * Copyright (c) 2004-2007 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2007-2013 Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2014-2019 Intel, Inc.  All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prte_config.h"

#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>

#include "constants.h"
#include "src/util/argv.h"
#include "src/util/printf.h"
#include "src/util/prte_environ.h"

#define PRTE_DEFAULT_TMPDIR "/tmp"

/*
 * Merge two environ-like char arrays, ensuring that there are no
 * duplicate entires
 */
char **prte_environ_merge(char **minor, char **major)
{
    int i;
    char **ret = NULL;
    char *name, *value;

    /* Check for bozo cases */

    if (NULL == major) {
        if (NULL == minor) {
            return NULL;
        } else {
            return prte_argv_copy(minor);
        }
    }

    /* First, copy major */

    ret = prte_argv_copy(major);

    /* Do we have something in minor? */

    if (NULL == minor) {
        return ret;
    }

    /* Now go through minor and call prte_setenv(), but with overwrite
       as false */

    for (i = 0; NULL != minor[i]; ++i) {
        value = strchr(minor[i], '=');
        if (NULL == value) {
            prte_setenv(minor[i], NULL, false, &ret);
        } else {

            /* strdup minor[i] in case it's a constat string */

            name = strdup(minor[i]);
            value = name + (value - minor[i]);
            *value = '\0';
            prte_setenv(name, value + 1, false, &ret);
            free(name);
        }
    }

    /* All done */

    return ret;
}

/*
 * Portable version of setenv(), allowing editing of any environ-like
 * array
 */
int prte_setenv(const char *name, const char *value, bool overwrite, char ***env)
{
    int i;
    char *newvalue, *compare;
    size_t len;

    /* Make the new value */

    if (NULL == value) {
        value = "";
        prte_asprintf(&newvalue, "%s=", name);
    } else {
        prte_asprintf(&newvalue, "%s=%s", name, value);
    }
    if (NULL == newvalue) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }

    /* Check the bozo case */

    if (NULL == env) {
        return PRTE_ERR_BAD_PARAM;
    } else if (NULL == *env) {
        i = 0;
        prte_argv_append(&i, env, newvalue);
        free(newvalue);
        return PRTE_SUCCESS;
    }

    /* If this is the "environ" array, use putenv */
    if (*env == environ) {
        /* THIS IS POTENTIALLY A MEMORY LEAK!  But I am doing it
           so that we don't violate the law of least
           astonishment for PRTE developers (i.e., those that don't
           check the return code of prte_setenv() and notice that we
           returned an error if you passed in the real environ) */
#if defined(HAVE_SETENV)
        setenv(name, value, overwrite);
        /* setenv copies the value, so we can free it here */
        free(newvalue);
#else
        len = strlen(name);
        for (i = 0; (*env)[i] != NULL; ++i) {
            if (0 == strncmp((*env)[i], name, len)) {
                /* if we find the value in the environ, then
                 * we need to check the overwrite flag to determine
                 * the correct response */
                if (overwrite) {
                    /* since it was okay to overwrite, do so */
                    putenv(newvalue);
                    /* putenv does NOT copy the value, so we
                     * cannot free it here */
                    return PRTE_SUCCESS;
                }
                /* since overwrite was not allowed, we return
                 * an error as we cannot perform the requested action */
                free(newvalue);
                return PRTE_EXISTS;
            }
        }
        /* since the value wasn't found, we can add it */
        putenv(newvalue);
        /* putenv does NOT copy the value, so we
         * cannot free it here */
#endif
        return PRTE_SUCCESS;
    }

    /* Make something easy to compare to */

    prte_asprintf(&compare, "%s=", name);
    if (NULL == compare) {
        free(newvalue);
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    len = strlen(compare);

    /* Look for a duplicate that's already set in the env */

    for (i = 0; (*env)[i] != NULL; ++i) {
        if (0 == strncmp((*env)[i], compare, len)) {
            if (overwrite) {
                free((*env)[i]);
                (*env)[i] = newvalue;
                free(compare);
                return PRTE_SUCCESS;
            } else {
                free(compare);
                free(newvalue);
                return PRTE_EXISTS;
            }
        }
    }

    /* If we found no match, append this value */

    i = prte_argv_count(*env);
    prte_argv_append(&i, env, newvalue);

    /* All done */

    free(compare);
    free(newvalue);
    return PRTE_SUCCESS;
}

/*
 * Portable version of unsetenv(), allowing editing of any
 * environ-like array
 */
int prte_unsetenv(const char *name, char ***env)
{
    int i;
    char *compare;
    size_t len;
    bool found;

    /* Check for bozo case */

    if (NULL == *env) {
        return PRTE_SUCCESS;
    }

    /* Make something easy to compare to */

    prte_asprintf(&compare, "%s=", name);
    if (NULL == compare) {
        return PRTE_ERR_OUT_OF_RESOURCE;
    }
    len = strlen(compare);

    /* Look for a duplicate that's already set in the env.  If we find
       it, free it, and then start shifting all elements down one in
       the array. */

    found = false;
    for (i = 0; (*env)[i] != NULL; ++i) {
        if (0 != strncmp((*env)[i], compare, len))
            continue;
        if (environ != *env) {
            free((*env)[i]);
        }
        for (; (*env)[i] != NULL; ++i)
            (*env)[i] = (*env)[i + 1];
        found = true;
        break;
    }
    free(compare);

    /* All done */

    return (found) ? PRTE_SUCCESS : PRTE_ERR_NOT_FOUND;
}

const char *prte_tmp_directory(void)
{
    const char *str;

    if (NULL == (str = getenv("TMPDIR")))
        if (NULL == (str = getenv("TEMP")))
            if (NULL == (str = getenv("TMP")))
                str = PRTE_DEFAULT_TMPDIR;
    return str;
}

const char *prte_home_directory(uid_t uid)
{
    const char *home = NULL;

    if (-1 == uid || uid == geteuid()) {
        home = getenv("HOME");
    }
    if (NULL == home) {
        struct passwd *pw = getpwuid(uid);
        home = pw->pw_dir;
    }

    return home;
}
