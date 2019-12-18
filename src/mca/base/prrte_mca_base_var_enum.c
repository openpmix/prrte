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
 * Copyright (c) 2012-2017 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2017      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017      IBM Corporation. All rights reserved.
 * Copyright (c) 2018      Amazon.com, Inc. or its affiliates.  All Rights reserved.
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "prrte_config.h"

#include "src/mca/base/prrte_mca_base_var_enum.h"
#include "src/mca/base/prrte_mca_base_vari.h"
#include "src/mca/base/base.h"
#include "src/util/argv.h"
#include "src/util/printf.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>

static void mca_base_var_enum_constructor (prrte_mca_base_var_enum_t *enumerator);
static void mca_base_var_enum_destructor (prrte_mca_base_var_enum_t *enumerator);
PRRTE_CLASS_INSTANCE(prrte_mca_base_var_enum_t, prrte_object_t, mca_base_var_enum_constructor,
                   mca_base_var_enum_destructor);

static void mca_base_var_enum_flag_constructor (prrte_mca_base_var_enum_flag_t *enumerator);
static void mca_base_var_enum_flag_destructor (prrte_mca_base_var_enum_flag_t *enumerator);
static PRRTE_CLASS_INSTANCE(prrte_mca_base_var_enum_flag_t, prrte_object_t, mca_base_var_enum_flag_constructor,
                   mca_base_var_enum_flag_destructor);

static int enum_dump (prrte_mca_base_var_enum_t *self, char **out);
static int enum_get_count (prrte_mca_base_var_enum_t *self, int *count);
static int enum_get_value (prrte_mca_base_var_enum_t *self, int index, int *value, const char **string_value);

static int mca_base_var_enum_bool_get_count (prrte_mca_base_var_enum_t *enumerator, int *count)
{
    *count = 2;
    return PRRTE_SUCCESS;
}

static int mca_base_var_enum_bool_get_value (prrte_mca_base_var_enum_t *self, int index,
                                             int *value, const char **string_value)
{
    if (1 < index) {
        return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
    }

    *value = index ? 1 : 0;
    *string_value = index ? "true" : "false";

    return PRRTE_SUCCESS;
}

static int mca_base_var_enum_bool_vfs (prrte_mca_base_var_enum_t *self, const char *string_value,
                                       int *value)
{
    char *tmp;
    long v;

    /* skip whitespace */
    string_value += strspn (string_value, " \t\n\v\f\r");

    v = strtol (string_value, &tmp, 10);
    if (*tmp != '\0') {
        if (0 == strcmp (string_value, "true") || 0 == strcmp (string_value, "t") ||
            0 == strcmp (string_value, "enabled") || 0 == strcmp (string_value, "yes") ||
            0 == strcmp (string_value, "y")) {
            v = 1;
        } else if (0 == strcmp (string_value, "false") || 0 == strcmp (string_value, "f") ||
                   0 == strcmp (string_value, "disabled") || 0 == strcmp (string_value, "no") ||
                   0 == strcmp (string_value, "n")) {
            v = 0;
        } else {
            return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
        }
    }

    *value = !!v;

    return PRRTE_SUCCESS;
}

static int mca_base_var_enum_bool_sfv (prrte_mca_base_var_enum_t *self, const int value,
                                       char **string_value)
{
    if (string_value) {
        *string_value = strdup (value ? "true" : "false");
    }

    return PRRTE_SUCCESS;
}

static int mca_base_var_enum_bool_dump (prrte_mca_base_var_enum_t *self, char **out)
{
    *out = strdup ("0: f|false|disabled|no|n, 1: t|true|enabled|yes|y");
    return *out ? PRRTE_SUCCESS : PRRTE_ERR_OUT_OF_RESOURCE;
}

prrte_mca_base_var_enum_t prrte_mca_base_var_enum_bool = {
    .super     = PRRTE_OBJ_STATIC_INIT(prrte_object_t),
    .enum_is_static = true,
    .enum_name = "boolean",
    .get_count = mca_base_var_enum_bool_get_count,
    .get_value = mca_base_var_enum_bool_get_value,
    .value_from_string = mca_base_var_enum_bool_vfs,
    .string_from_value = mca_base_var_enum_bool_sfv,
    .dump      = mca_base_var_enum_bool_dump
};

static int mca_base_var_enum_auto_bool_get_count (prrte_mca_base_var_enum_t *enumerator, int *count)
{
    *count = 3;
    return PRRTE_SUCCESS;
}

static int mca_base_var_enum_auto_bool_get_value (prrte_mca_base_var_enum_t *self, int index,
                                                  int *value, const char **string_value)
{
    const int values[3] = {0, 1, -1};
    const char *strings[3] = {"false", "true", "auto"};

    if (2 < index) {
        return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
    }

    *value = values[index];
    *string_value = strings[index];

    return PRRTE_SUCCESS;
}

static int mca_base_var_enum_auto_bool_vfs (prrte_mca_base_var_enum_t *self, const char *string_value,
                                            int *value)
{
    char *tmp;
    long v;

    /* skip whitespace */
    string_value += strspn (string_value, " \t\n\v\f\r");

    v = strtol (string_value, &tmp, 10);
    if (*tmp != '\0') {
        if (0 == strcasecmp (string_value, "true") || 0 == strcasecmp (string_value, "t") ||
            0 == strcasecmp (string_value, "enabled") || 0 == strcasecmp (string_value, "yes") ||
            0 == strcasecmp (string_value, "y")) {
            v = 1;
        } else if (0 == strcasecmp (string_value, "false") || 0 == strcasecmp (string_value, "f") ||
                   0 == strcasecmp (string_value, "disabled") || 0 == strcasecmp (string_value, "no") ||
                   0 == strcasecmp (string_value, "n")) {
            v = 0;
        } else if (0 == strcasecmp (string_value, "auto")) {
            v = -1;
        } else {
            return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
        }
    }

    if (v > 1) {
        *value = 1;
    } else if (v < -1) {
        *value = -1;
    } else {
        *value = (int) v;
    }

    return PRRTE_SUCCESS;
}

static int mca_base_var_enum_auto_bool_sfv (prrte_mca_base_var_enum_t *self, const int value,
                                            char **string_value)
{
    if (string_value) {
        if (value < 0) {
            *string_value = strdup ("auto");
        } else if (value > 0) {
            *string_value = strdup ("true");
        } else {
            *string_value = strdup ("false");
        }
    }

    return PRRTE_SUCCESS;
}

static int mca_base_var_enum_auto_bool_dump (prrte_mca_base_var_enum_t *self, char **out)
{
    *out = strdup ("-1: auto, 0: f|false|disabled|no|n, 1: t|true|enabled|yes|y");
    return *out ? PRRTE_SUCCESS : PRRTE_ERR_OUT_OF_RESOURCE;
}

prrte_mca_base_var_enum_t prrte_mca_base_var_enum_auto_bool = {
    .super     = PRRTE_OBJ_STATIC_INIT(prrte_object_t),
    .enum_is_static = true,
    .enum_name = "auto_boolean",
    .get_count = mca_base_var_enum_auto_bool_get_count,
    .get_value = mca_base_var_enum_auto_bool_get_value,
    .value_from_string = mca_base_var_enum_auto_bool_vfs,
    .string_from_value = mca_base_var_enum_auto_bool_sfv,
    .dump      = mca_base_var_enum_auto_bool_dump
};

/* verbosity enumerator */
static prrte_mca_base_var_enum_value_t verbose_values[] = {
    {PRRTE_MCA_BASE_VERBOSE_NONE,      "none"},
    {PRRTE_MCA_BASE_VERBOSE_ERROR,     "error"},
    {PRRTE_MCA_BASE_VERBOSE_COMPONENT, "component"},
    {PRRTE_MCA_BASE_VERBOSE_WARN,      "warn"},
    {PRRTE_MCA_BASE_VERBOSE_INFO,      "info"},
    {PRRTE_MCA_BASE_VERBOSE_TRACE,     "trace"},
    {PRRTE_MCA_BASE_VERBOSE_DEBUG,     "debug"},
    {PRRTE_MCA_BASE_VERBOSE_MAX,       "max"},
    {-1, NULL}
};

static int mca_base_var_enum_verbose_vfs (prrte_mca_base_var_enum_t *self, const char *string_value,
                                          int *value)
{
    char *tmp;
    int v;

    /* skip whitespace */
    string_value += strspn (string_value, " \t\n\v\f\r");

    v = strtol (string_value, &tmp, 10);
    if (*tmp != '\0') {
        for (int i = 0 ; verbose_values[i].string ; ++i) {
            if (0 == strcmp (verbose_values[i].string, string_value)) {
                *value = verbose_values[i].value;
                return PRRTE_SUCCESS;
            }
        }

        return PRRTE_ERR_NOT_FOUND;
    } else if (v < PRRTE_MCA_BASE_VERBOSE_NONE) {
        v = PRRTE_MCA_BASE_VERBOSE_NONE;
    } else if (v > PRRTE_MCA_BASE_VERBOSE_MAX) {
        v = PRRTE_MCA_BASE_VERBOSE_MAX;
    }

    *value = v;

    return PRRTE_SUCCESS;
}

static int mca_base_var_enum_verbose_sfv (prrte_mca_base_var_enum_t *self, const int value,
                                          char **string_value)
{
    int ret;

    if (value < 0 || value > 100) {
        return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
    }

    for (int i = 0 ; verbose_values[i].string ; ++i) {
        if (verbose_values[i].value == value) {
            if (string_value) {
                *string_value = strdup (verbose_values[i].string);
            }
            return PRRTE_SUCCESS;
        }
    }

    if (string_value) {
        ret = prrte_asprintf (string_value, "%d", value);
        if (0 > ret) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
    }

    return PRRTE_SUCCESS;
}

static int mca_base_var_enum_verbose_dump (prrte_mca_base_var_enum_t *self, char **out)
{
    char *tmp;
    int ret;

    ret = enum_dump (self, out);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    ret = prrte_asprintf (&tmp, "%s, 0 - 100", *out);
    free (*out);
    if (0 > ret) {
        *out = NULL;
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    *out = tmp;

    return PRRTE_SUCCESS;
}

prrte_mca_base_var_enum_t prrte_mca_base_var_enum_verbose = {
    .super     = PRRTE_OBJ_STATIC_INIT(prrte_object_t),
    .enum_is_static = true,
    .enum_name = "verbosity",
    .get_count = enum_get_count,
    .get_value = enum_get_value,
    .value_from_string = mca_base_var_enum_verbose_vfs,
    .string_from_value = mca_base_var_enum_verbose_sfv,
    .dump      = mca_base_var_enum_verbose_dump,
    .enum_value_count = 8,
    .enum_values = verbose_values,
};


int prrte_mca_base_var_enum_create (const char *name, const prrte_mca_base_var_enum_value_t *values, prrte_mca_base_var_enum_t **enumerator)
{
    prrte_mca_base_var_enum_t *new_enum;
    int i;

    *enumerator = NULL;

    new_enum = PRRTE_NEW(prrte_mca_base_var_enum_t);
    if (NULL == new_enum) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    new_enum->enum_name = strdup (name);
    if (NULL == new_enum->enum_name) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0 ; values[i].string ; ++i);
    new_enum->enum_value_count = i;

    /* make a copy of the values */
    new_enum->enum_values = calloc (new_enum->enum_value_count + 1, sizeof (*new_enum->enum_values));
    if (NULL == new_enum->enum_values) {
        PRRTE_RELEASE(new_enum);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0 ; i < new_enum->enum_value_count ; ++i) {
        new_enum->enum_values[i].value = values[i].value;
        new_enum->enum_values[i].string = strdup (values[i].string);
    }

    *enumerator = new_enum;

    return PRRTE_SUCCESS;
}

int prrte_mca_base_var_enum_create_flag (const char *name, const prrte_mca_base_var_enum_value_flag_t *flags, prrte_mca_base_var_enum_flag_t **enumerator)
{
    prrte_mca_base_var_enum_flag_t *new_enum;
    int i;

    *enumerator = NULL;

    new_enum = PRRTE_NEW(prrte_mca_base_var_enum_flag_t);
    if (NULL == new_enum) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    new_enum->super.enum_name = strdup (name);
    if (NULL == new_enum->super.enum_name) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    for (i = 0 ; flags[i].string ; ++i);
    new_enum->super.enum_value_count = i;

    /* make a copy of the values */
    new_enum->enum_flags = calloc (new_enum->super.enum_value_count + 1, sizeof (*new_enum->enum_flags));
    if (NULL == new_enum->enum_flags) {
        PRRTE_RELEASE(new_enum);
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    int all_flags = 0;
    for (i = 0 ; i < new_enum->super.enum_value_count ; ++i) {
        new_enum->enum_flags[i].flag = flags[i].flag;
        new_enum->enum_flags[i].string = strdup (flags[i].string);
        new_enum->enum_flags[i].conflicting_flag = flags[i].conflicting_flag;
        /* ensure flags are only set a single bit, doesn't conflict with itself, and
         * hasn't already been specified. */
        assert (!(flags[i].flag & (flags[i].flag - 1)));
        assert (!(flags[i].flag & flags[i].conflicting_flag));
        assert (!(all_flags & flags[i].flag));
        assert (flags[i].flag);
        all_flags |= flags[i].flag;
    }

    *enumerator = new_enum;

    return PRRTE_SUCCESS;
}

static int enum_dump (prrte_mca_base_var_enum_t *self, char **out)
{
    int i;
    char *tmp;
    int ret;

    *out = NULL;

    if (NULL == self) {
        return PRRTE_ERROR;
    }

    tmp = NULL;
    for (i = 0; i < self->enum_value_count && self->enum_values[i].string ; ++i) {
        ret = prrte_asprintf (out, "%s%s%d:\"%s\"", tmp ? tmp : "", tmp ? ", " : "", self->enum_values[i].value,
                        self->enum_values[i].string);
        if (tmp) free (tmp);
        if (0 > ret) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
        tmp = *out;
    }

    return PRRTE_SUCCESS;
}

static int enum_get_count (prrte_mca_base_var_enum_t *self, int *count)
{
    *count = self->enum_value_count;
    return PRRTE_SUCCESS;
}

static int enum_get_value (prrte_mca_base_var_enum_t *self, int index, int *value, const char **string_value)
{
    int count, ret;

    ret = self->get_count(self, &count);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    if (index >= count) {
        return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
    }

    if (value) {
        *value = self->enum_values[index].value;
    }

    if (string_value) {
        *string_value = strdup (self->enum_values[index].string);
    }

    return PRRTE_SUCCESS;
}

static int enum_value_from_string(prrte_mca_base_var_enum_t *self, const char *string_value, int *value_out) {
    int value, count, ret, i;
    bool is_int;
    char *tmp;

    ret = self->get_count(self, &count);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    value = strtol(string_value, &tmp, 0);

    /* Check if the string is an integer */
    is_int = tmp[0] == '\0';

    for (i = 0 ; i < count ; ++i) {
        if ((is_int && value == self->enum_values[i].value) ||
            0 == strcasecmp (string_value, self->enum_values[i].string)) {
            break;
        }
    }

    if (i == count) {
        return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
    }

    *value_out = self->enum_values[i].value;

    return PRRTE_SUCCESS;
}

static int enum_string_from_value(prrte_mca_base_var_enum_t *self, const int value, char **string_value) {
    int count, ret, i;

    ret = self->get_count(self, &count);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    for (i = 0 ; i < count ; ++i) {
        if (value == self->enum_values[i].value) {
            break;
        }
    }

    if (i == count) {
        return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
    }

    if (string_value) {
        *string_value = strdup (self->enum_values[i].string);
    }

    return PRRTE_SUCCESS;
}

static void mca_base_var_enum_constructor (prrte_mca_base_var_enum_t *enumerator)
{
    memset ((char *) enumerator + sizeof (enumerator->super), 0 , sizeof(*enumerator) - sizeof(enumerator->super));

    enumerator->get_value = enum_get_value;
    enumerator->get_count = enum_get_count;
    enumerator->value_from_string = enum_value_from_string;
    enumerator->string_from_value = enum_string_from_value;
    enumerator->dump      = enum_dump;
    enumerator->enum_is_static = false;
}

static void mca_base_var_enum_destructor (prrte_mca_base_var_enum_t *enumerator)
{
    if (enumerator->enum_name) {
        free (enumerator->enum_name);
    }

    /* release the copy of the values */
    if (enumerator->enum_values) {
        for (int i = 0 ; i < enumerator->enum_value_count ; ++i) {
            free ((void *) enumerator->enum_values[i].string);
        }
        free (enumerator->enum_values);
    }
}

static int enum_get_value_flag (prrte_mca_base_var_enum_t *self, int index, int *value, const char **string_value)
{
    prrte_mca_base_var_enum_flag_t *flag_enum = (prrte_mca_base_var_enum_flag_t *) self;
    int count, ret;

    ret = self->get_count(self, &count);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    if (index >= count) {
        return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
    }

    if (value) {
        *value = flag_enum->enum_flags[index].flag;
    }

    if (string_value) {
        *string_value = strdup (flag_enum->enum_flags[index].string);
    }

    return PRRTE_SUCCESS;
}

static int enum_value_from_string_flag (prrte_mca_base_var_enum_t *self, const char *string_value, int *value_out) {
    prrte_mca_base_var_enum_flag_t *flag_enum = (prrte_mca_base_var_enum_flag_t *) self;
    int value, count, ret, flag;
    char **flags;
    bool is_int;
    char *tmp;

    ret = self->get_count(self, &count);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    flags = prrte_argv_split (string_value, ',');
    if (NULL == flags) {
        return PRRTE_ERR_BAD_PARAM;
    }

    flag = 0;

    for (int i = 0 ; flags[i] ; ++i) {
        value = strtol (flags[i], &tmp, 0);
        is_int = tmp[0] == '\0';

        bool found = false, conflict = false;
        for (int j = 0 ; j < count ; ++j) {
            if ((is_int && (value & flag_enum->enum_flags[j].flag)) ||
                0 == strcasecmp (flags[i], flag_enum->enum_flags[j].string)) {
                found = true;

                if (flag & flag_enum->enum_flags[j].conflicting_flag) {
                    conflict = true;
                } else {
                    flag |= flag_enum->enum_flags[j].flag;
                }

                if (is_int) {
                    value &= ~flag_enum->enum_flags[j].flag;
                    if (0 == value) {
                        break;
                    }
                } else {
                    break;
                }
            }
        }

        if (!found || conflict || (is_int && value)) {
            prrte_argv_free (flags);
            return !found ? PRRTE_ERR_VALUE_OUT_OF_BOUNDS : PRRTE_ERR_BAD_PARAM;
        }
    }

    prrte_argv_free (flags);

    *value_out = flag;

    return PRRTE_SUCCESS;
}

static int enum_string_from_value_flag (prrte_mca_base_var_enum_t *self, const int value, char **string_value) {
    prrte_mca_base_var_enum_flag_t *flag_enum = (prrte_mca_base_var_enum_flag_t *) self;
    int count, ret, current;
    char *out = NULL, *tmp;

    ret = self->get_count(self, &count);
    if (PRRTE_SUCCESS != ret) {
        return ret;
    }

    current = value;
    for (int i = 0 ; i < count ; ++i) {
        if (!(flag_enum->enum_flags[i].flag & current)) {
            continue;
        }

        tmp = out;

        ret = prrte_asprintf (&out, "%s%s%s", tmp ? tmp : "", tmp ? "," : "", flag_enum->enum_flags[i].string);
        free (tmp);

        if (0 > ret) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }

        if (value & flag_enum->enum_flags[i].conflicting_flag) {
            free (out);
            return PRRTE_ERR_BAD_PARAM;
        }

        current &= ~flag_enum->enum_flags[i].flag;
    }

    if (current) {
        free (out);
        return PRRTE_ERR_VALUE_OUT_OF_BOUNDS;
    }

    if (string_value) {
        *string_value = out ? out : strdup ("");
    } else {
        free (out);
    }

    return PRRTE_SUCCESS;
}

static int enum_dump_flag (prrte_mca_base_var_enum_t *self, char **out)
{
    prrte_mca_base_var_enum_flag_t *flag_enum = (prrte_mca_base_var_enum_flag_t *) self;
    char *tmp;
    int ret;

    *out = NULL;

    if (NULL == self) {
        return PRRTE_ERROR;
    }

    *out = strdup ("Comma-delimited list of: ");
    if (NULL == *out) {
        return PRRTE_ERR_OUT_OF_RESOURCE;
    }

    for (int i = 0; i < self->enum_value_count ; ++i) {
        tmp = *out;

        ret = prrte_asprintf (out, "%s%s0x%x:\"%s\"", tmp, i ? ", " : " ", flag_enum->enum_flags[i].flag,
                        flag_enum->enum_flags[i].string);
        free (tmp);
        if (0 > ret) {
            return PRRTE_ERR_OUT_OF_RESOURCE;
        }
    }

    return PRRTE_SUCCESS;
}

static void mca_base_var_enum_flag_constructor (prrte_mca_base_var_enum_flag_t *enumerator)
{
    enumerator->enum_flags = NULL;
    enumerator->super.get_value = enum_get_value_flag;
    enumerator->super.get_count = enum_get_count;
    enumerator->super.value_from_string = enum_value_from_string_flag;
    enumerator->super.string_from_value = enum_string_from_value_flag;
    enumerator->super.dump = enum_dump_flag;
    enumerator->super.enum_is_static = false;
    enumerator->super.enum_name = NULL;
}

static void mca_base_var_enum_flag_destructor (prrte_mca_base_var_enum_flag_t *enumerator)
{
    /* release the copy of the values */
    if (enumerator->enum_flags) {
        for (int i = 0 ; i < enumerator->super.enum_value_count ; ++i) {
            free ((void *) enumerator->enum_flags[i].string);
        }
        free (enumerator->enum_flags);
    }
    if (NULL != enumerator->super.enum_name) {
        free (enumerator->super.enum_name);
    }
}

int prrte_mca_base_var_enum_register(const char *project_name, const char *framework_name,
                                     const char *component_name, const char *enum_name,
                                     void *storage)
{
    int group_index;

    /* Developer error. Storage can not be NULL */
    assert (NULL != storage);

    /* Create a new parameter entry */
    group_index = prrte_mca_base_var_group_register (project_name, framework_name, component_name, NULL);
    if (-1 > group_index) {
        return group_index;
    }

    if (0 <= group_index) {
        prrte_mca_base_var_group_add_enum (group_index, storage);
    }

    return PRRTE_SUCCESS;

    /* All done */
}
